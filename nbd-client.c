/*
 * Open connection for network block device
 *
 * Copyright 1997,1998 Pavel Machek, distribute under GPL
 *  <pavel@atrey.karlin.mff.cuni.cz>
 * Copyright (c) 2002 - 2011 Wouter Verhelst <w@uter.be>
 *
 * Version 1.0 - 64bit issues should be fixed, now
 * Version 1.1 - added bs (blocksize) option (Alexey Guzeev, aga@permonline.ru)
 * Version 1.2 - I added new option '-d' to send the disconnect request
 * Version 2.0 - Version synchronised with server
 * Version 2.1 - Check for disconnection before INIT_PASSWD is received
 * 	to make errormsg a bit more helpful in case the server can't
 * 	open the exported file.
 * 16/03/2010 - Add IPv6 support.
 * 	Kitt Tientanopajai <kitt@kitty.in.th>
 *	Neutron Soutmun <neo.neutron@gmail.com>
 *	Suriya Soutmun <darksolar@gmail.com>
 */

#include "config.h"
#include "lfs.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include "netdb-compat.h"
#include <stdio.h>
#include <fcntl.h>
#include <syslog.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

#include <linux/ioctl.h>

#ifdef HAVE_NETLINK
#include "nbd-netlink.h"
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#endif

#define MY_NAME "nbd_client"
#include "cliserv.h"

#ifdef WITH_SDP
#include <sdp_inet.h>
#endif

#define NBDC_DO_LIST 1

static int opennet(char *name, char* portstr, int sdp);
static int openunix(const char *path);
static void negotiate(int sock, u64 *rsize64, uint16_t *flags, char* name,
		      uint32_t needed_flags, uint32_t client_flags,
		      uint32_t do_opts);
#ifdef HAVE_NETLINK
static int netlink_index;

struct host_info {
	char *hostname;
	char *name;
	char *port;
	int sdp;
	int b_unix;
	int driver_id;
	int dead_timeout;
	struct nl_sock *socket;
};

static int callback(struct nl_msg *msg, void *arg) {
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *msg_attr[NBD_ATTR_MAX + 1];
	int ret;
	uint32_t index;

	ret = nla_parse(msg_attr, NBD_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
			genlmsg_attrlen(gnlh, 0), NULL);
	if (ret)
		err("Invalid response from the kernel\n");
	if (!msg_attr[NBD_ATTR_INDEX])
		err("Did not receive index from the kernel\n");
	index = nla_get_u32(msg_attr[NBD_ATTR_INDEX]);
	netlink_index = index;
	printf("Connected /dev/nbd%d\n", (int)index);
	return NL_OK;
}

static struct nl_sock *get_nbd_socket(int *driver_id) {
	struct nl_sock *socket;
	int id;

	socket = nl_socket_alloc();
	if (!socket)
		err("Couldn't allocate netlink socket\n");

	if (genl_connect(socket))
		err("Couldn't connect to the generic netlink socket\n");
	id = genl_ctrl_resolve(socket, "nbd");
	if (id < 0)
		err("Couldn't resolve the nbd netlink family, make sure the nbd module is loaded and your nbd driver supports the netlink interface.\n");
	if (driver_id)
		*driver_id = id;
	return socket;
}

static void netlink_configure(int index, int *sockfds, int num_connects,
			      u64 size64, int blocksize, uint16_t flags,
			      u64 client_flags, int timeout,
			      int dead_timeout) {
	struct nl_sock *socket;
	struct nlattr *sock_attr;
	struct nl_msg *msg;
	int driver_id, i;

	socket = get_nbd_socket(&driver_id);
	nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM, callback, NULL);

	msg = nlmsg_alloc();
	if (!msg)
		err("Couldn't allocate netlink message\n");
	genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, driver_id, 0, 0,
		    NBD_CMD_CONNECT, 0);
	if (index >= 0)
		NLA_PUT_U32(msg, NBD_ATTR_INDEX, index);
	NLA_PUT_U64(msg, NBD_ATTR_SIZE_BYTES, size64);
	NLA_PUT_U64(msg, NBD_ATTR_BLOCK_SIZE_BYTES, blocksize);
	NLA_PUT_U64(msg, NBD_ATTR_SERVER_FLAGS, flags);
	NLA_PUT_U64(msg, NBD_ATTR_TIMEOUT, timeout);
	NLA_PUT_U64(msg, NBD_ATTR_DEAD_CONN_TIMEOUT, dead_timeout);
	NLA_PUT_U64(msg, NBD_ATTR_CLIENT_FLAGS, client_flags);

	sock_attr = nla_nest_start(msg, NBD_ATTR_SOCKETS);
	if (!sock_attr)
		err("Couldn't nest the sockets for our connection\n");
	for (i = 0; i < num_connects; i++) {
		struct nlattr *sock_opt;
		sock_opt = nla_nest_start(msg, NBD_SOCK_ITEM);
		if (!sock_opt)
			err("Couldn't nest the sockets for our connection\n");
		NLA_PUT_U32(msg, NBD_SOCK_FD, sockfds[i]);
		nla_nest_end(msg, sock_opt);
	}
	nla_nest_end(msg, sock_attr);

	if (nl_send_sync(socket, msg) < 0)
		err("Failed to setup device, check dmesg\n");
	return;
nla_put_failure:
	err("Failed to create netlink message\n");
}

static void netlink_disconnect(char *nbddev) {
	struct nl_sock *socket;
	struct nlattr *sock_attr;
	struct nl_msg *msg;
	int driver_id, ret;

	int index = -1;
	if (nbddev) {
		if (sscanf(nbddev, "/dev/nbd%d", &index) != 1)
			err("Invalid nbd device target\n");
	}
	if (index < 0)
		err("Invalid nbd device target\n");

	socket = get_nbd_socket(&driver_id);

	msg = nlmsg_alloc();
	if (!msg)
		err("Couldn't allocate netlink message\n");
	genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, driver_id, 0, 0,
		    NBD_CMD_DISCONNECT, 0);
	NLA_PUT_U32(msg, NBD_ATTR_INDEX, index);
	if (nl_send_sync(socket, msg) < 0)
		err("Failed to disconnect device, check dmsg\n");
	nl_socket_free(socket);
	return;
nla_put_failure:
	err("Failed to create netlink message\n");
}

static int mcast_callback(struct nl_msg *msg, void *arg)
{
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *msg_attr[NBD_ATTR_MAX + 1];
	struct host_info *hinfo = (struct host_info *)arg;
	struct nlattr *sock_attr;
	struct nlattr *sock_opt;
	struct nl_msg *out_msg;
	uint32_t cflags = NBD_FLAG_C_FIXED_NEWSTYLE;
	uint16_t flags = 0;
	u64 size64 = 0;
	int ret;
	uint32_t index;
	int socket;
	int retries = 0;

	if (gnlh->cmd != NBD_CMD_LINK_DEAD)
		return NL_SKIP;
	ret = nla_parse(msg_attr, NBD_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
			genlmsg_attrlen(gnlh, 0), NULL);
	if (ret) {
		fprintf(stderr, "Invalid message from th kernel\n");
		return NL_SKIP;
	}
	if (!msg_attr[NBD_ATTR_INDEX]) {
		fprintf(stderr, "Don't have the index set\n");
		return NL_SKIP;
	}
	index = nla_get_u32(msg_attr[NBD_ATTR_INDEX]);

	printf("disconnect on index %d\n", index);
	do {
		if (hinfo->b_unix)
			socket = openunix(hinfo->hostname);
		else
			socket = opennet(hinfo->hostname, hinfo->port, hinfo->sdp);
		if (socket >= 0)
			break;
		sleep(1);
	} while (retries++ < hinfo->dead_timeout);

	if (socket < 0) {
		err_nonfatal("Couldn't reconnect to the server");
		return NL_OK;
	}
	negotiate(socket, &size64, &flags, hinfo->name, 0, cflags, 0);
	out_msg = nlmsg_alloc();
	if (!out_msg)
		err("Couldn't allocate netlink message\n");
	genlmsg_put(out_msg, NL_AUTO_PORT, NL_AUTO_SEQ, hinfo->driver_id, 0, 0,
		    NBD_CMD_RECONFIGURE, 0);
	NLA_PUT_U32(out_msg, NBD_ATTR_INDEX, index);
	sock_attr = nla_nest_start(out_msg, NBD_ATTR_SOCKETS);
	if (!sock_attr)
		err("Couldn't nest the sockets for our connection\n");
	sock_opt = nla_nest_start(out_msg, NBD_SOCK_ITEM);
	if (!sock_opt)
		err("Couldn't nest the sockets for our connection\n");
	NLA_PUT_U32(out_msg, NBD_SOCK_FD, socket);
	nla_nest_end(out_msg, sock_opt);
	nla_nest_end(out_msg, sock_attr);
	if (nl_send_sync(hinfo->socket, out_msg) < 0)
		err("Couldn't reconnect device\n");
	close(socket);
	return NL_OK;
nla_put_failure:
	err("Couldn't create the netlink message\n");
}

static void netlink_monitor(struct host_info *hinfo)
{
	struct nl_sock *sock = get_nbd_socket(NULL);
	struct nl_sock *tx_sock = get_nbd_socket(&hinfo->driver_id);
	int mcast_grp;

	mcast_grp = genl_ctrl_resolve_grp(sock, NBD_GENL_FAMILY_NAME,
					  NBD_GENL_MCAST_GROUP_NAME);
	if (mcast_grp < 0)
		err("Couldn't find the nbd multicast group\n");

	hinfo->socket = tx_sock;
	nl_socket_disable_seq_check(sock);
	nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, mcast_callback, hinfo);
	nl_socket_add_memberships(sock, mcast_grp, 0);
	while (1)
		nl_recvmsgs_default(sock);
}
#else
static void netlink_configure(int index, int *sockfds, int num_connects,
			      u64 size64, int blocksize, uint16_t flags,
			      int timeout)
{
}

static void netlink_disconnect(char *nbddev)
{
}
#endif /* HAVE_NETLINK */

static int check_conn(char* devname, int do_print) {
	char buf[256];
	char* p;
	int fd;
	int len;

	if( (p=strrchr(devname, '/')) ) {
		devname=p+1;
	}
	if((p=strchr(devname, 'p'))) {
		/* We can't do checks on partitions. */
		*p='\0';
	}
	snprintf(buf, 256, "/sys/block/%s/pid", devname);
	if((fd=open(buf, O_RDONLY))<0) {
		if(errno==ENOENT) {
			return 1;
		} else {
			return 2;
		}
	}
	len=read(fd, buf, 256);
	if(len < 0) {
		perror("could not read from server");
		close(fd);
		return 2;
	}
	buf[(len < 256) ? len : 255]='\0';
	if(do_print) printf("%s\n", buf);
	close(fd);
	return 0;
}

static int opennet(char *name, char* portstr, int sdp) {
	int sock;
	struct addrinfo hints;
	struct addrinfo *ai = NULL;
	struct addrinfo *rp = NULL;
	int e;

	memset(&hints,'\0',sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
	hints.ai_protocol = IPPROTO_TCP;

	e = getaddrinfo(name, portstr, &hints, &ai);

	if(e != 0) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(e));
		freeaddrinfo(ai);
		return -1;
	}

	if(sdp) {
#ifdef WITH_SDP
		if (ai->ai_family == AF_INET)
			ai->ai_family = AF_INET_SDP;
		else (ai->ai_family == AF_INET6)
			ai->ai_family = AF_INET6_SDP;
#else
		err("Can't do SDP: I was not compiled with SDP support!");
#endif
	}

	for(rp = ai; rp != NULL; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

		if(sock == -1)
			continue;	/* error */

		if(connect(sock, rp->ai_addr, rp->ai_addrlen) != -1)
			break;		/* success */
			
		close(sock);
	}

	if (rp == NULL) {
		err_nonfatal("Socket failed: %m");
		sock = -1;
		goto err;
	}

	setmysockopt(sock);
err:
	freeaddrinfo(ai);
	return sock;
}

static int openunix(const char *path) {
	int sock;
	struct sockaddr_un un_addr;
	memset(&un_addr, 0, sizeof(un_addr));

	un_addr.sun_family = AF_UNIX;
	if (strnlen(path, sizeof(un_addr.sun_path)) == sizeof(un_addr.sun_path)) {
		err_nonfatal("UNIX socket path too long");
		return -1;
	}

	strncpy(un_addr.sun_path, path, sizeof(un_addr.sun_path) - 1);

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		err_nonfatal("SOCKET failed");
		return -1;
	};

	if (connect(sock, &un_addr, sizeof(un_addr)) == -1) {
		err_nonfatal("CONNECT failed");
		close(sock);
		return -1;
	}
	return sock;
}

static void ask_list(int sock) {
	uint32_t opt;
	uint32_t opt_server;
	uint32_t len;
	uint32_t reptype;
	uint64_t magic;
	int rlen;
	const int BUF_SIZE = 1024;
	char buf[BUF_SIZE];

	magic = ntohll(opts_magic);
	if (write(sock, &magic, sizeof(magic)) < 0)
		err("Failed/2.2: %m");

	/* Ask for the list */
	opt = htonl(NBD_OPT_LIST);
	if(write(sock, &opt, sizeof(opt)) < 0) {
		err("writing list option failed: %m");
	}
	/* Send the length (zero) */
	len = htonl(0);
	if(write(sock, &len, sizeof(len)) < 0) {
		err("writing length failed: %m");
	}
	/* newline, move away from the "Negotiation:" line */
	printf("\n");
	do {
		memset(buf, 0, 1024);
		if(read(sock, &magic, sizeof(magic)) < 0) {
			err("Reading magic from server: %m");
		}
		if(read(sock, &opt_server, sizeof(opt_server)) < 0) {
			err("Reading option: %m");
		}
		if(read(sock, &reptype, sizeof(reptype)) <0) {
			err("Reading reply from server: %m");
		}
		if(read(sock, &len, sizeof(len)) < 0) {
			err("Reading length from server: %m");
		}
		magic=ntohll(magic);
		len=ntohl(len);
		reptype=ntohl(reptype);
		if(magic != rep_magic) {
			err("Not enough magic from server");
		}
		if(reptype & NBD_REP_FLAG_ERROR) {
			switch(reptype) {
				case NBD_REP_ERR_POLICY:
					fprintf(stderr, "\nE: listing not allowed by server.\n");
					break;
				default:
					fprintf(stderr, "\nE: unexpected error from server.\n");
					break;
			}
			if(len > 0 && len < BUF_SIZE) {
				if((rlen=read(sock, buf, len)) < 0) {
					fprintf(stderr, "\nE: could not read error message from server\n");
				} else {
					buf[rlen] = '\0';
					fprintf(stderr, "Server said: %s\n", buf);
				}
			}
			exit(EXIT_FAILURE);
		} else {
			if(len) {
				if(reptype != NBD_REP_SERVER) {
					err("Server sent us a reply we don't understand!");
				}
				if(read(sock, &len, sizeof(len)) < 0) {
					fprintf(stderr, "\nE: could not read export name length from server\n");
					exit(EXIT_FAILURE);
				}
				len=ntohl(len);
				if (len >= BUF_SIZE) {
					fprintf(stderr, "\nE: export name on server too long\n");
					exit(EXIT_FAILURE);
				}
				if(read(sock, buf, len) < 0) {
					fprintf(stderr, "\nE: could not read export name from server\n");
					exit(EXIT_FAILURE);
				}
				buf[len] = 0;
				printf("%s\n", buf);
			}
		}
	} while(reptype != NBD_REP_ACK);
	opt=htonl(NBD_OPT_ABORT);
	len=htonl(0);
	magic=htonll(opts_magic);
	if (write(sock, &magic, sizeof(magic)) < 0)
		err("Failed/2.2: %m");
	if (write(sock, &opt, sizeof(opt)) < 0)
		err("Failed writing abort");
	if (write(sock, &len, sizeof(len)) < 0)
		err("Failed writing length");
}

static void negotiate(int sock, u64 *rsize64, uint16_t *flags, char* name,
		      uint32_t needed_flags, uint32_t client_flags,
		      uint32_t do_opts)
{
	u64 magic, size64;
	uint16_t tmp;
	uint16_t global_flags;
	char buf[256] = "\0\0\0\0\0\0\0\0\0";
	uint32_t opt;
	uint32_t namesize;

	printf("Negotiation: ");
	readit(sock, buf, 8);
	if (strcmp(buf, INIT_PASSWD))
		err("INIT_PASSWD bad");
	printf(".");
	readit(sock, &magic, sizeof(magic));
	magic = ntohll(magic);
	if (magic != opts_magic) {
		if(magic == cliserv_magic) {
			err("It looks like you're trying to connect to an oldstyle server. This is no longer supported since nbd 3.10.");
		}
	}
	printf(".");
	readit(sock, &tmp, sizeof(uint16_t));
	global_flags = ntohs(tmp);
	if((needed_flags & global_flags) != needed_flags) {
		/* There's currently really only one reason why this
		 * check could possibly fail, but we may need to change
		 * this error message in the future... */
		fprintf(stderr, "\nE: Server does not support listing exports\n");
		exit(EXIT_FAILURE);
	}

	if (global_flags & NBD_FLAG_NO_ZEROES) {
		client_flags |= NBD_FLAG_C_NO_ZEROES;
	}
	client_flags = htonl(client_flags);
	if (write(sock, &client_flags, sizeof(client_flags)) < 0)
		err("Failed/2.1: %m");

	if(do_opts & NBDC_DO_LIST) {
		ask_list(sock);
		exit(EXIT_SUCCESS);
	}

	/* Write the export name that we're after */
	magic = htonll(opts_magic);
	if (write(sock, &magic, sizeof(magic)) < 0)
		err("Failed/2.2: %m");

	opt = ntohl(NBD_OPT_EXPORT_NAME);
	if (write(sock, &opt, sizeof(opt)) < 0)
		err("Failed/2.3: %m");
	namesize = (u32)strlen(name);
	namesize = ntohl(namesize);
	if (write(sock, &namesize, sizeof(namesize)) < 0)
		err("Failed/2.4: %m");
	if (write(sock, name, strlen(name)) < 0)
		err("Failed/2.4: %m");

	readit(sock, &size64, sizeof(size64));
	size64 = ntohll(size64);

	if ((size64>>12) > (uint64_t)~0UL) {
		printf("size = %luMB", (unsigned long)(size64>>20));
		err("Exported device is too big for me. Get 64-bit machine :-(\n");
	} else
		printf("size = %luMB", (unsigned long)(size64>>20));

	readit(sock, &tmp, sizeof(tmp));
	*flags = (uint32_t)ntohs(tmp);

	if (!(global_flags & NBD_FLAG_NO_ZEROES)) {
		readit(sock, &buf, 124);
	}
	printf("\n");

	*rsize64 = size64;
}

static bool get_from_config(char* cfgname, char** name_ptr, char** dev_ptr,
			    char** hostn_ptr, int* bs, int* timeout,
			    int* persist, int* swap, int* sdp, int* b_unix,
			    char**port) {
	int fd = open(SYSCONFDIR "/nbdtab", O_RDONLY);
	bool retval = false;
	if(fd < 0) {
		fprintf(stderr, "while opening %s: ", SYSCONFDIR "/nbdtab");
		perror("could not open config file");
		goto out;
	}
	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	void *data = NULL;
	char *fsep = "\n\t# ";
	char *lsep = "\n#";

	if(size < 0) {
		perror("E: mmap'ing nbdtab");
		exit(EXIT_FAILURE);
	}

	data = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
	if(!strncmp(cfgname, "/dev/", 5)) {
		cfgname += 5;
	}
	char *loc = strstr((const char*)data, cfgname);
	if(!loc) {
		goto out;
	}
	size_t l = strlen(cfgname) + 6;
	*dev_ptr = malloc(l);
	snprintf(*dev_ptr, l, "/dev/%s", cfgname);

	size_t line_len, field_len, ws_len;
#define CHECK_LEN field_len = strcspn(loc, fsep); ws_len = strspn(loc+field_len, fsep); if(field_len > line_len || line_len <= 0) { goto out; }
#define MOVE_NEXT line_len -= field_len + ws_len; loc += field_len + ws_len
	// find length of line
	line_len = strcspn(loc, lsep);
	// first field is the device node name, which we already know, so skip it
	CHECK_LEN;
	MOVE_NEXT;
	// next field is the hostname
	CHECK_LEN;
	*hostn_ptr = strndup(loc, field_len);
	MOVE_NEXT;
	// third field is the export name
	CHECK_LEN;
	*name_ptr = strndup(loc, field_len);
	if(ws_len + field_len > line_len) {
		// optional last field is not there, so return success
		retval = true;
		goto out;
	}
	MOVE_NEXT;
	CHECK_LEN;
#undef CHECK_LEN
#undef MOVE_NEXT
	// fourth field is the options field, a comma-separated field of options
	do {
		if(!strncmp(loc, "bs=", 3)) {
			*bs = (int)strtol(loc+3, &loc, 0);
			goto next;
		}
		if(!strncmp(loc, "timeout=", 8)) {
			*timeout = (int)strtol(loc+8, &loc, 0);
			goto next;
		}
		if(!strncmp(loc, "port=", 5)) {
			*port = strndup(loc+5, strcspn(loc+5, ","));
			goto next;
		}
		if(!strncmp(loc, "persist", 7)) {
			loc += 7;
			*persist = 1;
			goto next;
		}
		if(!strncmp(loc, "swap", 4)) {
			*swap = 1;
			loc += 4;
			goto next;
		}
		if(!strncmp(loc, "sdp", 3)) {
			*sdp = 1;
			loc += 3;
			goto next;
		}
		if(!strncmp(loc, "unix", 4)) {
			*b_unix = 1;
			loc += 4;
			goto next;
		}
		// skip unknown options, with a warning unless they start with a '_'
		l = strcspn(loc, ",");
		if(*loc != '_') {
			char* s = strndup(loc, l);
			fprintf(stderr, "Warning: unknown option '%s' found in nbdtab file", s);
			free(s);
		}
		loc += l;
next:
		if(*loc == ',') {
			loc++;
		}
	} while(strcspn(loc, lsep) > 0);
	retval = true;
out:
	if(data != NULL) {
		munmap(data, size);
	}
	if(fd >= 0) {
		close(fd);
	}
	return retval;
}

static void setsizes(int nbd, u64 size64, int blocksize, u32 flags) {
	unsigned long size;
	int read_only = (flags & NBD_FLAG_READ_ONLY) ? 1 : 0;

	if (size64>>12 > (uint64_t)~0UL)
		err("Device too large.\n");
	else {
		int tmp_blocksize = 4096;
		if (size64 / (u64)blocksize <= (uint64_t)~0UL)
			tmp_blocksize = blocksize;
		if (ioctl(nbd, NBD_SET_BLKSIZE, tmp_blocksize) < 0) {
			fprintf(stderr, "Failed to set blocksize %d\n",
				tmp_blocksize);
			err("Ioctl/1.1a failed: %m\n");
		}
		size = (unsigned long)(size64 / (u64)tmp_blocksize);
		if (ioctl(nbd, NBD_SET_SIZE_BLOCKS, size) < 0)
			err("Ioctl/1.1b failed: %m\n");
		if (tmp_blocksize != blocksize) {
			if (ioctl(nbd, NBD_SET_BLKSIZE, (unsigned long)blocksize) < 0) {
				fprintf(stderr, "Failed to set blocksize %d\n",
					blocksize);
				err("Ioctl/1.1c failed: %m\n");
			}
		}
		fprintf(stderr, "bs=%d, sz=%llu bytes\n", tmp_blocksize, (u64)tmp_blocksize * size);
	}

	ioctl(nbd, NBD_CLEAR_SOCK);

	/* ignore error as kernel may not support */
	ioctl(nbd, NBD_SET_FLAGS, (unsigned long) flags);

	if (ioctl(nbd, BLKROSET, (unsigned long) &read_only) < 0)
		err("Unable to set read-only attribute for device");
}

static void set_timeout(int nbd, int timeout) {
	if (timeout) {
		if (ioctl(nbd, NBD_SET_TIMEOUT, (unsigned long)timeout) < 0)
			err("Ioctl NBD_SET_TIMEOUT failed: %m\n");
		fprintf(stderr, "timeout=%d\n", timeout);
	}
}

static void finish_sock(int sock, int nbd, int swap) {
	if (ioctl(nbd, NBD_SET_SOCK, sock) < 0) {
		if (errno == EBUSY)
			err("Kernel doesn't support multiple connections\n");
		else
			err("Ioctl NBD_SET_SOCK failed: %m\n");
	}

#ifndef __ANDROID__
	if (swap)
		mlockall(MCL_CURRENT | MCL_FUTURE);
#endif
}

static int
oom_adjust(const char *file, const char *value)
{
	int fd, rc;
	size_t len;

	fd = open(file, O_WRONLY);
	if (fd < 0)
		return -1;
	len = strlen(value);
	rc = write(fd, value, len) != (ssize_t) len;
	close(fd);
	return rc ? -1 : 0;
}

static void usage(char* errmsg, ...) {
	if(errmsg) {
		char tmp[256];
		va_list ap;
		va_start(ap, errmsg);
		snprintf(tmp, 256, "ERROR: %s\n\n", errmsg);
		vfprintf(stderr, tmp, ap);
		va_end(ap);
	} else {
		fprintf(stderr, "nbd-client version %s\n", PACKAGE_VERSION);
	}
#ifdef HAVE_NETLINK
	fprintf(stderr, "Usage: nbd-client -name|-N name host [port] nbd_device\n\t[-block-size|-b block size] [-timeout|-t timeout] [-swap|-s] [-sdp|-S]\n\t[-persist|-p] [-nofork|-n] [-systemd-mark|-m] -L\n");
#else
	fprintf(stderr, "Usage: nbd-client -name|-N name host [port] nbd_device\n\t[-block-size|-b block size] [-timeout|-t timeout] [-swap|-s] [-sdp|-S]\n\t[-persist|-p] [-nofork|-n] [-systemd-mark|-m]\n");
#endif
	fprintf(stderr, "Or   : nbd-client -u (with same arguments as above)\n");
	fprintf(stderr, "Or   : nbd-client nbdX\n");
	fprintf(stderr, "Or   : nbd-client -d nbd_device\n");
	fprintf(stderr, "Or   : nbd-client -c nbd_device\n");
	fprintf(stderr, "Or   : nbd-client -h|--help\n");
	fprintf(stderr, "Or   : nbd-client -l|--list host\n");
	fprintf(stderr, "Default value for blocksize is 1024 (recommended for ethernet)\n");
	fprintf(stderr, "Allowed values for blocksize are 512,1024,2048,4096\n"); /* will be checked in kernel :) */
	fprintf(stderr, "Note, that kernel 2.4.2 and older ones do not work correctly with\n");
	fprintf(stderr, "blocksizes other than 1024 without patches\n");
	fprintf(stderr, "Default value for port is 10809. Note that port must always be numeric\n");
}

static void disconnect(char* device) {
	int nbd = open(device, O_RDWR);

	if (nbd < 0)
		err("Cannot open NBD: %m\nPlease ensure the 'nbd' module is loaded.");
	printf("disconnect, ");
	if (ioctl(nbd, NBD_DISCONNECT)<0)
		err("Ioctl failed: %m\n");
	printf("sock, ");
	if (ioctl(nbd, NBD_CLEAR_SOCK)<0)
		err("Ioctl failed: %m\n");
	printf("done\n");
}

#ifdef HAVE_NETLINK
static const char *short_opts = "-b:c:C:d:D:ehlLMnN:pSst:u";
#else
static const char *short_opts = "-b:c:C:d:hlnN:pSst:u";
#endif

int main(int argc, char *argv[]) {
	char* port=NBD_DEFAULT_PORT;
	int sock, nbd;
	int blocksize=1024;
	char *hostname=NULL;
	char *nbddev=NULL;
	int swap=0;
	int cont=0;
	int timeout=30;
	int sdp=0;
	int G_GNUC_UNUSED nofork=0; // if -dNOFORK
	pid_t main_pid;
	u64 size64;
	u64 client_flags = 0;
	uint16_t flags = 0;
	int c;
	int nonspecial=0;
	int b_unix=0;
	char* name="";
	uint16_t needed_flags=0;
	uint32_t cflags=NBD_FLAG_C_FIXED_NEWSTYLE;
	uint32_t opts=0;
	sigset_t block, old;
	struct sigaction sa;
	int num_connections = 1;
	int netlink = 0;
	int monitor = 0;
	int need_disconnect = 0;
	int dead_timeout = 0;
	int *sockfds;
	struct option long_options[] = {
		{ "block-size", required_argument, NULL, 'b' },
		{ "check", required_argument, NULL, 'c' },
		{ "connections", required_argument, NULL, 'C'},
		{ "disconnect", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ "list", no_argument, NULL, 'l' },
		{ "monitor", no_argument, NULL, 'M'},
		{ "name", required_argument, NULL, 'N' },
#ifdef HAVE_NETLINK
		{ "netlink", no_argument, NULL, 'L' },
		{ "dead-timeout", required_argument, NULL, 'D'},
		{ "destroy", no_argument, NULL, 'e'},
#endif
		{ "nofork", no_argument, NULL, 'n' },
		{ "persist", no_argument, NULL, 'p' },
		{ "sdp", no_argument, NULL, 'S' },
		{ "swap", no_argument, NULL, 's' },
		{ "systemd-mark", no_argument, NULL, 'm' },
		{ "timeout", required_argument, NULL, 't' },
		{ "unix", no_argument, NULL, 'u' },
		{ 0, 0, 0, 0 }, 
	};
	int i;

	logging(MY_NAME);

	while((c=getopt_long_only(argc, argv, short_opts, long_options, NULL))>=0) {
		switch(c) {
		case 1:
			// non-option argument
			if(strchr(optarg, '=')) {
				// old-style 'bs=' or 'timeout='
				// argument
				fprintf(stderr, "WARNING: old-style command-line argument encountered. This is deprecated.\n");
				if(!strncmp(optarg, "bs=", 3)) {
					optarg+=3;
					goto blocksize;
				}
				if(!strncmp(optarg, "timeout=", 8)) {
					optarg+=8;
					goto timeout;
				}
				usage("unknown option %s encountered", optarg);
				exit(EXIT_FAILURE);
			}
			switch(nonspecial++) {
				case 0:
					// host
					hostname=optarg;
					break;
				case 1:
					// port
					if(!strtol(optarg, NULL, 0)) {
						// not parseable as a number, assume it's the device
						nbddev = optarg;
						nonspecial++;
					} else {
						port = optarg;
					}
					break;
				case 2:
					// device
					nbddev = optarg;
					break;
				default:
					usage("too many non-option arguments specified");
					exit(EXIT_FAILURE);
			}
			break;
		case 'b':
		      blocksize:
			blocksize=(int)strtol(optarg, NULL, 0);
			break;
		case 'c':
			return check_conn(optarg, 1);
		case 'C':
			num_connections = (int)strtol(optarg, NULL, 0);
			break;
		case 'd':
			need_disconnect = 1;
			nbddev = strdup(optarg);
			break;
		case 'h':
			usage(NULL);
			exit(EXIT_SUCCESS);
		case 'l':
			needed_flags |= NBD_FLAG_FIXED_NEWSTYLE;
			opts |= NBDC_DO_LIST;
			nbddev="";
			break;
#ifdef HAVE_NETLINK
		case 'L':
			netlink = 1;
			break;
		case 'M':
			monitor = 1;
			netlink = 1;
			break;
		case 'D':
			netlink = 1;
			dead_timeout = (int)strtol(optarg, NULL, 0);
			break;
		case 'e':
			client_flags |= NBD_CFLAG_DESTROY_ON_DISCONNECT;
			break;
#endif
		case 'm':
			argv[0][0] = '@';
			break;
		case 'n':
			nofork=1;
			break;
		case 'N':
			name=optarg;
			break;
		case 'p':
			cont=1;
			break;
		case 's':
			swap=1;
			break;
		case 'S':
			sdp=1;
			break;
		case 't':
		      timeout:
			timeout=strtol(optarg, NULL, 0);
			break;
		case 'u':
			b_unix = 1;
			break;
		default:
			fprintf(stderr, "E: option eaten by 42 mice\n");
			exit(EXIT_FAILURE);
		}
	}

	if (need_disconnect) {
		if (netlink)
			netlink_disconnect(nbddev);
		else
			disconnect(nbddev);
		exit(EXIT_SUCCESS);
	}
#ifdef __ANDROID__
  if (swap)
    err("swap option unsupported on Android because mlockall is unsupported.");
#endif
	if(hostname) {
		if((!name || !nbddev) && !(opts & NBDC_DO_LIST)) {
			if(!strncmp(hostname, "nbd", 3) || !strncmp(hostname, "/dev/nbd", 8)) {
				if(!get_from_config(hostname, &name, &nbddev, &hostname, &blocksize, &timeout, &cont, &swap, &sdp, &b_unix, &port)) {
					usage("no valid configuration for specified device found", hostname);
					exit(EXIT_FAILURE);
				}
			} else if (!netlink) {
				usage("not enough information specified, and argument didn't look like an nbd device");
				exit(EXIT_FAILURE);
			}
		}
	} else {
		usage("no information specified");
		exit(EXIT_FAILURE);
	}

	if (netlink)
		nofork = 1;

	if(strlen(name)==0 && !(opts & NBDC_DO_LIST)) {
		printf("Warning: the oldstyle protocol is no longer supported.\nThis method now uses the newstyle protocol with a default export\n");
	}

	if (!netlink) {
		nbd = open(nbddev, O_RDWR);
		if (nbd < 0)
		  err("Cannot open NBD: %m\nPlease ensure the 'nbd' module is loaded.");
	}

	if (netlink) {
		sockfds = malloc(sizeof(int) * num_connections);
		if (!sockfds)
			err("Cannot allocate the socket fd's array");
	}

	for (i = 0; i < num_connections; i++) {
		if (b_unix)
			sock = openunix(hostname);
		else
			sock = opennet(hostname, port, sdp);
		if (sock < 0)
			exit(EXIT_FAILURE);

		negotiate(sock, &size64, &flags, name, needed_flags, cflags, opts);
		if (netlink) {
			sockfds[i] = sock;
			continue;
		}

		if (i == 0) {
			setsizes(nbd, size64, blocksize, flags);
			set_timeout(nbd, timeout);
		}
		finish_sock(sock, nbd, swap);
		if (swap) {
			/* try linux >= 2.6.36 interface first */
			if (oom_adjust("/proc/self/oom_score_adj", "-1000")) {
				/* fall back to linux <= 2.6.35 interface */
				oom_adjust("/proc/self/oom_adj", "-17");
			}
		}
	}

	if (netlink) {
		int index = -1;
		if (nbddev) {
			if (sscanf(nbddev, "/dev/nbd%d", &index) != 1)
				err("Invalid nbd device target\n");
		}
		netlink_configure(index, sockfds, num_connections,
				  size64, blocksize, flags, client_flags,
				  timeout, dead_timeout);
		free(sockfds);
		if (monitor) {
			struct host_info info;

			info.hostname = hostname;
			info.name = name;
			info.port = port;
			info.sdp = sdp;
			info.b_unix = b_unix;
			info.dead_timeout = dead_timeout;
			netlink_monitor(&info);
		}
		return 0;
	}
	/* Go daemon */
	
#ifndef NOFORK
	if(!nofork) {
		if (daemon(0,0) < 0)
			err("Cannot detach from terminal");
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
#endif
	/* For child to check its parent */
	main_pid = getpid();
	do {
#ifndef NOFORK

		sigfillset(&block);
		sigdelset(&block, SIGKILL);
		sigdelset(&block, SIGTERM);
		sigdelset(&block, SIGPIPE);
		sigprocmask(SIG_SETMASK, &block, &old);

		if (!fork()) {
			/* Due to a race, the kernel NBD driver cannot
			 * call for a reread of the partition table
			 * in the handling of the NBD_DO_IT ioctl().
			 * Therefore, this is done in the first open()
			 * of the device. We therefore make sure that
			 * the device is opened at least once after the
			 * connection was made. This has to be done in a
			 * separate process, since the NBD_DO_IT ioctl()
			 * does not return until the NBD device has
			 * disconnected.
			 */
			struct timespec req = {
				.tv_sec = 0,
				.tv_nsec = 100000000,
			};
			while(check_conn(nbddev, 0)) {
				if (main_pid != getppid()) {
					/* check_conn() will not return 0 when nbd disconnected
					 * and parent exited during this loop. So the child has to
					 * explicitly check parent identity and exit if parent
					 * exited */
					exit(0);
				}
				nanosleep(&req, NULL);
			}
			open(nbddev, O_RDONLY);
			exit(0);
		}
#endif

		if (ioctl(nbd, NBD_DO_IT) < 0) {
			int error = errno;
			fprintf(stderr, "nbd,%d: Kernel call returned: %d", main_pid, error);
			if(error==EBADR) {
				/* The user probably did 'nbd-client -d' on us.
				 * quit */
				cont=0;
			} else {
				if(cont) {
					u64 new_size;
					uint16_t new_flags;

					close(sock); close(nbd);
					for (;;) {
						fprintf(stderr, " Reconnecting\n");
						if (b_unix)
							sock = openunix(hostname);
						else
							sock = opennet(hostname, port, sdp);
						if (sock >= 0)
							break;
						sleep (1);
					}
					nbd = open(nbddev, O_RDWR);
					if (nbd < 0)
						err("Cannot open NBD: %m");
					negotiate(sock, &new_size, &new_flags, name, needed_flags, cflags, opts);
					if (size64 != new_size) {
						err("Size of the device changed. Bye");
					}
					setsizes(nbd, size64, blocksize,
								new_flags);

					set_timeout(nbd, timeout);
					finish_sock(sock,nbd,swap);
				}
			}
		} else {
			/* We're on 2.4. It's not clearly defined what exactly
			 * happened at this point. Probably best to quit, now
			 */
			fprintf(stderr, "Kernel call returned.");
			cont=0;
		}
	} while(cont);
	printf("sock, ");
	ioctl(nbd, NBD_CLEAR_SOCK);
	printf("done\n");
	return 0;
}
