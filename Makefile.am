SUBDIRS = . man doc tests systemd gznbd
bin_PROGRAMS = nbd-server nbd-trdump
sbin_PROGRAMS = @NBD_CLIENT_NAME@
EXTRA_PROGRAMS = nbd-client make-integrityhuge
noinst_LTLIBRARIES = libnbdsrv.la libcliserv.la
libcliserv_la_SOURCES = cliserv.h cliserv.c
libcliserv_la_CFLAGS = @CFLAGS@
nbd_client_SOURCES = nbd-client.c cliserv.h
nbd_server_SOURCES = nbd-server.c cliserv.h lfs.h nbd.h nbdsrv.h backend.h
nbd_trdump_SOURCES = nbd-trdump.c cliserv.h nbd.h
nbd_server_CFLAGS = @CFLAGS@ @GLIB_CFLAGS@
nbd_trdump_CFLAGS = @CFLAGS@ @GLIB_CFLAGS@
libnbdsrv_la_SOURCES = nbdsrv.c nbdsrv.h treefiles.c treefiles.h
libnbdsrv_la_CFLAGS = @CFLAGS@ @GLIB_CFLAGS@
nbd_client_LDADD = libcliserv.la
nbd_server_LDADD = @GLIB_LIBS@ libnbdsrv.la libcliserv.la
nbd_trdump_LDADD = libcliserv.la
make_integrityhuge_SOURCES = make-integrityhuge.c cliserv.h nbd.h nbd-debug.h
EXTRA_DIST = maketr CodingStyle autogen.sh README.md

if NETLINK
bin_PROGRAMS += nbd-get-status
nbd_get_status_SOURCES = nbd-get-status.c
nbd_get_status_CFLAGS = @CFLAGS@
endif
