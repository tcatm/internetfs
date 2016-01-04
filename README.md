# internetfs (bittorrent filesystem)

## Example usage

    $ src/btfs /mount/point
    $ cd /mount/point/c3e36bb32d7b8f1b89dcb003575f89ec859cf198/
    ...wait a few moments
    $ ls
    debian-8.2.0-amd64-netinst.iso

To unmount and shutdown:

    $ fusermount -u /mount/point

## Dependencies (on Linux)

* fuse ("fuse" in Debian/Ubuntu)
* libtorrent ("libtorrent-rasterbar7" in Debian/Ubuntu)

## Building from git on a recent Debian/Ubuntu

    $ sudo apt-get install autoconf automake libfuse-dev libtorrent-rasterbar-dev
    $ cd internetfs
    $ autoreconf -i
    $ ./configure
    $ make

And optionally, if you want to install it:

    $ sudo make install

