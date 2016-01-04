/*
Copyright 2015 Johan Gunnarsson <johan.gunnarsson@gmail.com>
          2016 Nils Schneider <nils@nilsschneider.net>

This file is part of BTFS.

BTFS is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

BTFS is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with BTFS.  If not, see <http://www.gnu.org/licenses/>.
*/

#define FUSE_USE_VERSION 26

#include <cstdlib>

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <fuse.h>

#include <libtorrent/torrent_info.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/peer_request.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>

#include "btfs.h"
#include "torrent.h"

#define RETV(s, v) { s; return v; };

using namespace btfs;

libtorrent::session *session = NULL;

pthread_t alert_thread;

std::list<Read*> reads;

// First piece index of the current sliding window
int cursor;

std::map<std::string,int> files;
std::map<std::string,std::set<std::string> > dirs;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t signal_cond = PTHREAD_COND_INITIALIZER;

static struct btfs_params params;

static bool
move_to_next_unfinished(libtorrent::torrent_handle &handle, int& piece) {
	for (; piece < handle.get_torrent_info().num_pieces(); piece++) {
		if (!handle.have_piece(piece))
			return true;
	}

	return false;
}

static void
jump(libtorrent::torrent_handle &handle, int piece, int size) {
	int tail = piece;

	if (!move_to_next_unfinished(handle, tail))
		return;

	cursor = tail;

	int pl = handle.get_torrent_info().piece_length();

	for (int b = 0; b < 16 * pl; b += pl) {
		handle.piece_priority(tail++, 7);
	}

	for (int o = (tail - piece) * pl; o < size + pl - 1; o += pl) {
		handle.piece_priority(tail++, 1);
	}
}

static void
advance(libtorrent::torrent_handle &handle) {
	jump(handle, cursor, 0);
}

Read::Read(libtorrent::torrent_handle &h, char *buf, int index, int offset, int size) {
  handle = h;

	libtorrent::torrent_info metadata = handle.get_torrent_info();

	libtorrent::file_entry file = metadata.file_at(index);

	while (size > 0 && offset < file.size) {
		libtorrent::peer_request part = metadata.map_file(index,
			offset, size);

		part.length = std::min(
			metadata.piece_size(part.piece) - part.start,
			part.length);

		parts.push_back(Part(part, buf));

		size -= part.length;
		offset += part.length;
		buf += part.length;
	}
}

void Read::copy(int piece, char *buffer, int size) {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if (i->part.piece == piece && !i->filled)
			i->filled = (memcpy(i->buf, buffer + i->part.start,
				i->part.length)) != NULL;
	}
}

void Read::trigger() {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if (handle.have_piece(i->part.piece))
			handle.read_piece(i->part.piece);
	}
}

bool Read::finished() {
	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		if (!i->filled)
			return false;
	}

	return true;
}

int Read::size() {
	int s = 0;

	for (parts_iter i = parts.begin(); i != parts.end(); ++i) {
		s += i->part.length;
	}

	return s;
}

int Read::read() {
	if (size() <= 0)
		return 0;

	// Trigger reads of finished pieces
	trigger();

	// Move sliding window to first piece to serve this request
	jump(handle, parts.front().part.piece, size());

	while (!finished())
		// Wait for any piece to downloaded
		pthread_cond_wait(&signal_cond, &lock);

	return size();
}

static bool
populate_target(libtorrent::add_torrent_params& p, char *arg) {
	std::string templ;

	if (arg) {
		templ += arg;
	} else if (getenv("HOME")) {
		templ += getenv("HOME");
		templ += "/btfs";
	} else {
		templ += "/tmp/btfs";
	}

	if (mkdir(templ.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0) {
		if (errno != EEXIST)
			RETV(fprintf(stderr, "Failed to create target: %m\n"),
				false);
	}

	templ += "/btfs-XXXXXX";

	char *s = strdup(templ.c_str());

	if (mkdtemp(s) != NULL) {
		char *x = realpath(s, NULL);

		if (x)
			p.save_path = x;
		else
			perror("Failed to expand target");

		free(x);
	} else {
		perror("Failed to generate target");
	}

	free(s);

	return p.save_path.length() > 0;
}

static void
add_info_hash(libtorrent::sha1_hash &hash) {
  printf("adding info hash %s\n", libtorrent::to_hex(hash.to_string()).c_str());
	libtorrent::add_torrent_params p;

  populate_target(p, NULL);

	p.flags &= ~libtorrent::add_torrent_params::flag_auto_managed;
	p.flags &= ~libtorrent::add_torrent_params::flag_paused;

  p.info_hash = hash;

	session->async_add_torrent(p);
}

static void
setup(libtorrent::torrent_handle &handle) {
	printf("Got metadata. Now ready to start downloading.\n");

	libtorrent::torrent_info ti = handle.get_torrent_info();

  std::string info_hash = libtorrent::to_hex(handle.info_hash().to_string());

	for (int i = 0; i < ti.num_files(); ++i) {
		// Initially, don't download anything
		handle.file_priority(i, 0);

		std::string parent("");

		char *p = strdup(ti.file_at(i).path.c_str());

		for (char *x = strtok(p, "/"); x; x = strtok(NULL, "/")) {
			if (strlen(x) <= 0)
				continue;

			if (parent.length() <= 0)
				// Root dir <-> children mapping
				dirs["/" + info_hash].insert(x);
			else
				// Non-root dir <-> children mapping
		 		dirs["/" + info_hash + parent].insert(x);

			parent += "/";
			parent += x;
		}

		free(p);

		// Path <-> file index mapping
		files["/" + info_hash + "/" + ti.file_at(i).path] = i;
	}
}

static void
handle_read_piece_alert(libtorrent::read_piece_alert *a) {
	printf("%s: piece %d size %d\n", __func__, a->piece, a->size);

	pthread_mutex_lock(&lock);

	for (reads_iter i = reads.begin(); i != reads.end(); ++i) {
		(*i)->copy(a->piece, a->buffer.get(), a->size);
	}

	pthread_mutex_unlock(&lock);

	// Wake up all threads waiting for download
	pthread_cond_broadcast(&signal_cond);
}

static void
handle_piece_finished_alert(libtorrent::piece_finished_alert *a) {
	printf("%s: %d\n", __func__, a->piece_index);

	pthread_mutex_lock(&lock);

	for (reads_iter i = reads.begin(); i != reads.end(); ++i) {
		(*i)->trigger();
	}

	// Advance sliding window
	advance(a->handle);

	pthread_mutex_unlock(&lock);
}

static void
handle_metadata_failed_alert(libtorrent::metadata_failed_alert *a) {
	//printf("%s\n", __func__);
}

static void
handle_torrent_added_alert(libtorrent::torrent_added_alert *a) {
	//printf("%s()\n", __func__);

	pthread_mutex_lock(&lock);

	if (a->handle.status(0).has_metadata)
		setup(a->handle);

	pthread_mutex_unlock(&lock);
}

static void
handle_metadata_received_alert(libtorrent::metadata_received_alert *a) {
	//printf("%s\n", __func__);

	pthread_mutex_lock(&lock);

	setup(a->handle);

	pthread_mutex_unlock(&lock);
}

static void
handle_alert(libtorrent::alert *a) {
	switch (a->type()) {
	case libtorrent::read_piece_alert::alert_type:
		handle_read_piece_alert(
			(libtorrent::read_piece_alert *) a);
		break;
	case libtorrent::piece_finished_alert::alert_type:
		handle_piece_finished_alert(
			(libtorrent::piece_finished_alert *) a);
		break;
	case libtorrent::metadata_failed_alert::alert_type:
		handle_metadata_failed_alert(
			(libtorrent::metadata_failed_alert *) a);
		break;
	case libtorrent::metadata_received_alert::alert_type:
		handle_metadata_received_alert(
			(libtorrent::metadata_received_alert *) a);
		break;
	case libtorrent::torrent_added_alert::alert_type:
		handle_torrent_added_alert(
			(libtorrent::torrent_added_alert *) a);
		break;
	default:
		//printf("unknown event %d\n", a->type());
		break;
	}

	delete a;
}

static void*
alert_queue_loop(void *data) {
	int oldstate, oldtype;

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);

	while (1) {
		if (!session->wait_for_alert(libtorrent::seconds(1)))
			continue;

		std::deque<libtorrent::alert*> alerts;

		session->pop_alerts(&alerts);

		std::for_each(alerts.begin(), alerts.end(), handle_alert);
	}

	return NULL;
}

static bool
is_dir(const char *path) {
	return dirs.find(path) != dirs.end();
}

static bool
is_file(const char *path) {
	return files.find(path) != files.end();
}

static int
btfs_getattr(const char *path, struct stat *stbuf) {
  char *pathdup = strdupa(path);
  char *saveptr;
  char *info_hash_str = strtok_r(pathdup, "/", &saveptr);

  memset(stbuf, 0, sizeof (*stbuf));

  stbuf->st_uid = getuid();
  stbuf->st_gid = getgid();

  if (info_hash_str != NULL) {
    pathdup = strdupa(path);
    char *remainder = strchr(pathdup + 1, '/');

    if (strlen(info_hash_str) != 40)
      return -ENOENT;

    if (strspn(info_hash_str, "0123456789abcdef") != 40)
      return -ENOENT;

    char sha1[20];
    libtorrent::from_hex(info_hash_str, 40, sha1);
    libtorrent::sha1_hash info_hash(sha1);

    libtorrent::torrent_handle torrent = session->find_torrent(info_hash);

    if (!torrent.is_valid())
      add_info_hash(info_hash);

    if (remainder == NULL)
      stbuf->st_mode = S_IFDIR | 0755;
    else {
      if (!torrent.is_valid())
        return -ENOENT;

      // info_hash auswerten um torrent zu finden
      if (!is_dir(path) && !is_file(path))
        return -ENOENT;

      pthread_mutex_lock(&lock);

      if (is_dir(path)) {
        stbuf->st_mode = S_IFDIR | 0755;
      } else {
        libtorrent::file_entry file =
          torrent.get_torrent_info().file_at(files[path]);

        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_size = file.size;
      }

      pthread_mutex_unlock(&lock);
    }
  } else {
    stbuf->st_mode = S_IFDIR | 0755;
  }

	return 0;
}

static int
btfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
  // /info_hash/$foo
  // /info_hash muss 40 zeichen hex haben
  // from_hex() aufrufen
  // sha1_assign() aufrufen
  // schauen ob find_torrents ihn kennt, sonst hinzufügen
  //
  char *pathdup = strdupa(path);
  char *saveptr;
  char *info_hash_str = strtok_r(pathdup, "/", &saveptr);

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  if (info_hash_str != NULL) {
    pathdup = strdupa(path);
    char *remainder = strchr(pathdup + 1, '/');

    if (strlen(info_hash_str) != 40)
      return -ENOENT;

    if (strspn(info_hash_str, "0123456789abcdef") != 40)
      return -ENOENT;

    if (remainder != NULL) {
      if (!is_dir(path))
        return -ENOENT;

      if (is_file(path))
        return -ENOTDIR;
    }

    pthread_mutex_lock(&lock);

    for (std::set<std::string>::iterator i = dirs[path].begin(); i != dirs[path].end(); ++i) {
      filler(buf, i->c_str(), NULL, 0);
    }

    pthread_mutex_unlock(&lock);
  } else {
    std::vector<libtorrent::torrent_handle> torrents = session->get_torrents();
    for (std::vector<libtorrent::torrent_handle>::iterator i = torrents.begin(); i != torrents.end(); ++i)
      filler(buf, libtorrent::to_hex(i->info_hash().to_string()).c_str(), NULL, 0);
  }

	return 0;
}

static int
btfs_open(const char *path, struct fuse_file_info *fi) {
	if (!is_dir(path) && !is_file(path))
		return -ENOENT;

	if (is_dir(path))
		return -EISDIR;

	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int
btfs_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	//printf("%s: %s %lu %ld\n", __func__, path, size, offset);

	if (!is_dir(path) && !is_file(path))
		return -ENOENT;

	if (is_dir(path))
		return -EISDIR;

	pthread_mutex_lock(&lock);

  char *pathdup = strdupa(path);
  char *saveptr;
  char *info_hash_str = strtok_r(pathdup, "/", &saveptr);

  if (info_hash_str == NULL)
    return -ENOENT;

  if (strlen(info_hash_str) != 40)
    return -ENOENT;

  if (strspn(info_hash_str, "0123456789abcdef") != 40)
    return -ENOENT;

  char sha1[20];
  libtorrent::from_hex(info_hash_str, 40, sha1);
  libtorrent::sha1_hash info_hash(sha1);

  libtorrent::torrent_handle handle = session->find_torrent(info_hash);

  if (!handle.is_valid())
    return -ENOENT;

	Read *r = new Read(handle, buf, files[path], offset, size);

	reads.push_back(r);

	// Wait for read to finish
	int s = r->read();

	reads.remove(r);

	delete r;

	pthread_mutex_unlock(&lock);

	return s;
}

static void *
btfs_init(struct fuse_conn_info *conn) {
	pthread_mutex_lock(&lock);

	libtorrent::add_torrent_params *p = (libtorrent::add_torrent_params *)
		fuse_get_context()->private_data;

	int alerts =
		//libtorrent::alert::all_categories |
		libtorrent::alert::storage_notification |
		libtorrent::alert::progress_notification |
		libtorrent::alert::status_notification |
		libtorrent::alert::error_notification;

	session = new libtorrent::session(
		libtorrent::fingerprint(
			"LT",
			LIBTORRENT_VERSION_MAJOR,
			LIBTORRENT_VERSION_MINOR,
			0,
			0),
		std::make_pair(6881, 6889),
		NULL,
		libtorrent::session::add_default_plugins,
		alerts);

  session->start_dht();
  session->start_lsd();

  session->add_dht_router(std::make_pair("router.bittorrent.com", 6881));
  session->add_dht_router(std::make_pair("router.utorrent.com", 6881));

	pthread_create(&alert_thread, NULL, alert_queue_loop, NULL);

#ifndef __APPLE__
	pthread_setname_np(alert_thread, "alert");
#endif

	libtorrent::session_settings se = session->settings();

	se.strict_end_game_mode = false;
	se.announce_to_all_trackers = true;
	se.announce_to_all_tiers = true;

	session->set_settings(se);

	pthread_mutex_unlock(&lock);

	return NULL;
}

static void
btfs_destroy(void *user_data) {
	pthread_mutex_lock(&lock);

	pthread_cancel(alert_thread);
	pthread_join(alert_thread, NULL);

  std::vector<libtorrent::torrent_handle> torrents = session->get_torrents();
  for (std::vector<libtorrent::torrent_handle>::iterator i = torrents.begin(); i != torrents.end(); ++i) {
  	session->remove_torrent(*i, params.keep ? 0 : libtorrent::session::delete_files);
	  rmdir(i->save_path().c_str());
  }

	delete session;

	pthread_mutex_unlock(&lock);
}

static size_t
handle_http(void *contents, size_t size, size_t nmemb, void *userp) {
	Array *output = (Array *) userp;

	// Offset into buffer to write to
	size_t off = output->size;

	output->expand(nmemb * size);

	memcpy(output->buf + off, contents, nmemb * size);

	// Must return number of bytes copied
	return nmemb * size;
}

#define BTFS_OPT(t, p, v) { t, offsetof(struct btfs_params, p), v }

static const struct fuse_opt btfs_opts[] = {
	BTFS_OPT("-v",            version,     1),
	BTFS_OPT("--version",     version,     1),
	BTFS_OPT("-h",            help,        1),
	BTFS_OPT("--help",        help,        1),
	BTFS_OPT("-k",            keep,        1),
	BTFS_OPT("--keep",        keep,        1),
	FUSE_OPT_END
};

int
main(int argc, char *argv[]) {
	struct fuse_operations btfs_ops;
	memset(&btfs_ops, 0, sizeof (btfs_ops));

	btfs_ops.getattr = btfs_getattr;
	btfs_ops.readdir = btfs_readdir;
	btfs_ops.open = btfs_open;
	btfs_ops.read = btfs_read;
	btfs_ops.init = btfs_init;
	btfs_ops.destroy = btfs_destroy;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	if (fuse_opt_parse(&args, &params, btfs_opts, NULL))
		RETV(fprintf(stderr, "Failed to parse options\n"), -1);

	if (params.version) {
		// Print version
		printf(PACKAGE " version: " VERSION "\n");

		// Let FUSE print more versions
		fuse_opt_add_arg(&args, "--version");
		fuse_main(args.argc, args.argv, &btfs_ops, NULL);

		return 0;
	}

	if (params.help) {
		// Print usage
		printf("usage: " PACKAGE " [options] mountpoint\n");
		printf("\n");
		printf("btfs options:\n");
		printf("    --version -v           show version information\n");
		printf("    --help -h              show this message\n");
		printf("    --keep -k              keep files after unmount\n");
		printf("\n");

		// Let FUSE print more help
		fuse_opt_add_arg(&args, "-ho");
		fuse_main(args.argc, args.argv, &btfs_ops, NULL);

		return 0;
	}

	fuse_main(args.argc, args.argv, &btfs_ops, NULL);

	return 0;
}
