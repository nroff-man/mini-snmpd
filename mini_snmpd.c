/* Main program
 *
 * Copyright (C) 2008-2010  Robert Ernst <robert.ernst@linux-solutions.at>
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See COPYING for GPL licensing information.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include "mini_snmpd.h"


static void print_help(void)
{
	printf("Mini snmpd v" VERSION " -- Minimal SNMP daemon for UNIX systems\n"
	       "\n"
	       "Usage: mini_snmpd [options]\n"
	       "\n"
#ifdef CONFIG_ENABLE_IPV6
	       "  -4, --use-ipv4                  Use IPv4, default\n"
	       "  -6, --use-ipv6                  Use IPv6\n"
#endif
#ifdef HAVE_LIBCONFUSE
	       "  -f, --file=FILE                 Configuration file. Default: " CONFDIR "/%s.conf\n"
#endif
	       "  -p, --udp-port PORT             UDP port to bind to, default: 161\n"
	       "  -P, --tcp-port PORT             TCP port to bind to, default: 161\n"
	       "  -c, --community STR             Community string, default: public\n"
	       "  -D, --description STR           System description, default: none\n"
	       "  -V, --vendor OID                System vendor, default: none\n"
	       "  -L, --location STR              System location, default: none\n"
	       "  -C, --contact STR               System contact, default: none\n"
	       "  -d, --disks PATH                Disks to monitor, default: /\n"
	       "  -i, --interfaces IFACE          Network interfaces to monitor, default: none\n"
#ifdef __linux__
	       "  -w, --wireless-interfaces IFACE Wireless network interfaces to monitor, default: none\n"
#endif
	       "  -I, --listen IFACE              Network interface to listen, default: all\n"
	       "  -t, --timeout SEC               Timeout for MIB updates, default: 1 second\n"
	       "  -a, --auth                      Enable authentication, i.e. SNMP version 2c\n"
	       "  -n, --foreground                Run in foreground, do not detach from controlling terminal\n"
	       "  -s, --syslog                    Use syslog for logging, even if running in the foreground\n"
	       "  -v, --verbose                   Verbose messages\n"
	       "  -h, --help                      This help text\n"
	       "\n"
#ifdef HAVE_LIBCONFUSE
	       , PACKAGE_NAME
#endif
		);
}

static void handle_signal(int UNUSED(signo))
{
	g_quit = 1;
}

static void handle_udp_client(void)
{
	const char *req_msg = "Failed UDP request from";
	const char *snd_msg = "Failed UDP response to";
	ssize_t rv;
	char straddr[my_inet_addrstrlen] = "";
	my_socklen_t socklen;
	struct my_sockaddr_t sockaddr;

	/* Read the whole UDP packet from the socket at once */
	socklen = sizeof(sockaddr);
	rv = recvfrom(g_udp_sockfd, g_udp_client.packet, sizeof(g_udp_client.packet),
		      0, (struct sockaddr *)&sockaddr, &socklen);
	if (rv == -1) {
		lprintf(LOG_WARNING, "Failed receiving UDP request on port %d: %m\n", g_udp_port);
		return;
	}

	g_udp_client.timestamp = time(NULL);
	g_udp_client.sockfd = g_udp_sockfd;
	g_udp_client.addr = sockaddr.my_sin_addr;
	g_udp_client.port = sockaddr.my_sin_port;
	g_udp_client.size = rv;
	g_udp_client.outgoing = 0;
#ifdef DEBUG
	dump_packet(&g_udp_client);
#endif

	/* Call the protocol handler which will prepare the response packet */
	inet_ntop(my_af_inet, &sockaddr.my_sin_addr, straddr, sizeof(straddr));
	if (snmp(&g_udp_client) == -1) {
		lprintf(LOG_WARNING, "%s %s:%d: %m\n", req_msg, straddr, sockaddr.my_sin_port);
		return;
	}
	if (g_udp_client.size == 0) {
		lprintf(LOG_WARNING, "%s %s:%d: ignored\n", req_msg, straddr, sockaddr.my_sin_port);
		return;
	}
	g_udp_client.outgoing = 1;

	/* Send the whole UDP packet to the socket at once */
	rv = sendto(g_udp_sockfd, g_udp_client.packet, g_udp_client.size,
		MSG_DONTWAIT, (struct sockaddr *)&sockaddr, socklen);
	inet_ntop(my_af_inet, &sockaddr.my_sin_addr, straddr, sizeof(straddr));
	if (rv == -1)
		lprintf(LOG_WARNING, "%s %s:%d: %m\n", snd_msg, straddr, sockaddr.my_sin_port);
	else if ((size_t)rv != g_udp_client.size)
		lprintf(LOG_WARNING, "%s %s:%d: only %zd of %zu bytes sent\n", snd_msg, straddr, sockaddr.my_sin_port, rv, g_udp_client.size);

#ifdef DEBUG
	dump_packet(&g_udp_client);
#endif
}

static void handle_tcp_connect(void)
{
	int rv;
	const char *msg = "Could not accept TCP connection";
	char straddr[my_inet_addrstrlen] = "";
	client_t *client;
	my_socklen_t socklen;
	struct my_sockaddr_t tmp_sockaddr;
	struct my_sockaddr_t sockaddr;

	/* Accept the new connection (remember the client's IP address and port) */
	socklen = sizeof(sockaddr);
	rv = accept(g_tcp_sockfd, (struct sockaddr *)&sockaddr, &socklen);
	if (rv == -1) {
		lprintf(LOG_ERR, "%s: %m\n", msg);
		return;
	}
	if (rv >= FD_SETSIZE) {
		lprintf(LOG_ERR, "%s: FD set overflow\n", msg);
		close(rv);
		return;
	}

	/* Create a new client control structure or overwrite the oldest one */
	if (g_tcp_client_list_length >= MAX_NR_CLIENTS) {
		client = find_oldest_client();
		if (!client) {
			lprintf(LOG_ERR, "%s: internal error", msg);
			exit(EXIT_SYSCALL);
		}

		tmp_sockaddr.my_sin_addr = client->addr;
		tmp_sockaddr.my_sin_port = client->port;
		inet_ntop(my_af_inet, &tmp_sockaddr.my_sin_addr, straddr, sizeof(straddr));
		lprintf(LOG_WARNING, "Maximum number of %d clients reached, kicking out %s:%d\n",
			MAX_NR_CLIENTS, straddr, tmp_sockaddr.my_sin_port);
		close(client->sockfd);
	} else {
		client = allocate(sizeof(client_t));
		if (!client) {
			lprintf(LOG_ERR, "%s: %m", msg);
			exit(EXIT_SYSCALL);
		}
		g_tcp_client_list[g_tcp_client_list_length++] = client;
	}

	/* Now fill out the client control structure values */
	inet_ntop(my_af_inet, &sockaddr.my_sin_addr, straddr, sizeof(straddr));
	lprintf(LOG_DEBUG, "Connected TCP client %s:%d\n",
		straddr, sockaddr.my_sin_port);
	client->timestamp = time(NULL);
	client->sockfd = rv;
	client->addr = sockaddr.my_sin_addr;
	client->port = sockaddr.my_sin_port;
	client->size = 0;
	client->outgoing = 0;
}

static void handle_tcp_client_write(client_t *client)
{
	const char *msg = "Failed TCP response to";
	ssize_t rv;
	char straddr[my_inet_addrstrlen] = "";
	struct my_sockaddr_t sockaddr;

	/* Send the packet atomically and close socket if that did not work */
	sockaddr.my_sin_addr = client->addr;
	sockaddr.my_sin_port = client->port;
	rv = send(client->sockfd, client->packet, client->size, 0);
	inet_ntop(my_af_inet, &sockaddr.my_sin_addr, straddr, sizeof(straddr));
	if (rv == -1) {
		lprintf(LOG_WARNING, "%s %s:%d: %m\n", msg, straddr, sockaddr.my_sin_port);
		close(client->sockfd);
		client->sockfd = -1;
		return;
	}
	if ((size_t)rv != client->size) {
		lprintf(LOG_WARNING, "%s %s:%d: only %zd of %zu bytes written\n",
			msg, straddr, sockaddr.my_sin_port, rv, client->size);
		close(client->sockfd);
		client->sockfd = -1;
		return;
	}

#ifdef DEBUG
	dump_packet(client);
#endif

	/* Put the client into listening mode again */
	client->size = 0;
	client->outgoing = 0;
}

static void handle_tcp_client_read(client_t *client)
{
	const char *req_msg = "Failed TCP request from";
	int rv;
	char straddr[my_inet_addrstrlen] = "";
	struct my_sockaddr_t sockaddr;

	/* Read from the socket what arrived and put it into the buffer */
	sockaddr.my_sin_addr = client->addr;
	sockaddr.my_sin_port = client->port;
	rv = read(client->sockfd, client->packet + client->size, sizeof(client->packet) - client->size);
	inet_ntop(my_af_inet, &sockaddr.my_sin_addr, straddr, sizeof(straddr));
	if (rv == -1) {
		lprintf(LOG_WARNING, "%s %s:%d: %m\n", req_msg, straddr, sockaddr.my_sin_port);
		close(client->sockfd);
		client->sockfd = -1;
		return;
	}
	if (rv == 0) {
		lprintf(LOG_DEBUG, "TCP client %s:%d disconnected\n",
			straddr, sockaddr.my_sin_port);
		close(client->sockfd);
		client->sockfd = -1;
		return;
	}
	client->timestamp = time(NULL);
	client->size += rv;

	/* Check whether the packet was fully received and handle packet if yes */
	rv = snmp_packet_complete(client);
	if (rv == -1) {
		lprintf(LOG_WARNING, "%s %s:%d: %m\n", req_msg, straddr, sockaddr.my_sin_port);
		close(client->sockfd);
		client->sockfd = -1;
		return;
	}
	if (rv == 0) {
		return;
	}
	client->outgoing = 0;

#ifdef DEBUG
	dump_packet(client);
#endif

	/* Call the protocol handler which will prepare the response packet */
	if (snmp(client) == -1) {
		lprintf(LOG_WARNING, "%s %s:%d: %m\n", req_msg, straddr, sockaddr.my_sin_port);
		close(client->sockfd);
		client->sockfd = -1;
		return;
	}
	if (client->size == 0) {
		lprintf(LOG_WARNING, "%s %s:%d: ignored\n", req_msg, straddr, sockaddr.my_sin_port);
		close(client->sockfd);
		client->sockfd = -1;
		return;
	}

	client->outgoing = 1;
}


int main(int argc, char *argv[])
{
	static const char short_options[] = "p:P:c:D:V:L:C:d:i:w:t:ansvh"
#ifndef __FreeBSD__
		"I:"
#endif
#ifdef CONFIG_ENABLE_IPV6
		"46"
#endif
#ifdef HAVE_LIBCONFUSE
		"f:"
#endif
		;
	static const struct option long_options[] = {
#ifdef CONFIG_ENABLE_IPV6
		{ "use-ipv4", 0, 0, '4' },
		{ "use-ipv6", 0, 0, '6' },
#endif
#ifdef HAVE_LIBCONFUSE
		{ "file",     1, 0, 'f' },
#endif
		{ "udp-port", 1, 0, 'p' },
		{ "tcp-port", 1, 0, 'P' },
		{ "community", 1, 0, 'c' },
		{ "description", 1, 0, 'D' },
		{ "vendor", 1, 0, 'V' },
		{ "location", 1, 0, 'L' },
		{ "contact", 1, 0, 'C' },
		{ "disks", 1, 0, 'd' },
		{ "interfaces", 1, 0, 'i' },
#ifdef __linux__
		{ "wireless-interfaces", 1, 0, 'w' },
#endif
#ifndef __FreeBSD__
		{ "listen", 1, 0, 'I' },
#endif
		{ "timeout", 1, 0, 't' },
		{ "auth", 0, 0, 'a' },
		{ "foreground", 0, 0, 'n' },
		{ "verbose", 0, 0, 'v' },
		{ "syslog", 0, 0, 's' },
		{ "help", 0, 0, 'h' },
		{ NULL, 0, 0, 0 }
	};
	int ticks, nfds, c, option_index = 1;
	size_t i;
	fd_set rfds, wfds;
	struct sigaction sig;
	struct ifreq ifreq;
	struct timeval tv_last;
	struct timeval tv_now;
	struct timeval tv_sleep;
	my_socklen_t socklen;
	union {
		struct sockaddr_in sa;
#ifdef CONFIG_ENABLE_IPV6
		struct sockaddr_in6 sa6;
#endif
	} sockaddr;
#ifdef HAVE_LIBCONFUSE
	char path[256] = "";
	char *config = NULL;
#endif

	/* Prevent TERM and HUP signals from interrupting system calls */
	sig.sa_handler = handle_signal;
	sigemptyset (&sig.sa_mask);
	sig.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &sig, NULL);
	sigaction(SIGHUP, &sig, NULL);

	/* Parse commandline options */
	while (1) {
		c = getopt_long(argc, argv, short_options, long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
#ifdef CONFIG_ENABLE_IPV6
			case '4':
				g_family = AF_INET;
				break;

			case '6':
				g_family = AF_INET6;
				break;
#endif
#ifdef HAVE_LIBCONFUSE
			case 'f':
				config = optarg;
				break;
#endif

			case 'p':
				g_udp_port = atoi(optarg);
				break;

			case 'P':
				g_tcp_port = atoi(optarg);
				break;

			case 'c':
				g_community = strdup(optarg);
				break;

			case 'D':
				g_description = strdup(optarg);
				break;

			case 'V':
				g_vendor = strdup(optarg);
				break;

			case 'L':
				g_location = strdup(optarg);
				break;

			case 'C':
				g_contact = strdup(optarg);
				break;
#ifndef __FreeBSD__
			case 'I':
				g_bind_to_device = strdup(optarg);
				break;
#endif
			case 'd':
				g_disk_list_length = split(optarg, ",:;", g_disk_list, MAX_NR_DISKS);
				break;

			case 'i':
				g_interface_list_length = split(optarg, ",;", g_interface_list, MAX_NR_INTERFACES);
				break;
#ifdef __linux__
			case 'w':
				g_wireless_list_length = split(optarg, ",;", g_wireless_list, MAX_NR_INTERFACES);
				break;
#endif
			case 't':
				g_timeout = atoi(optarg) * 100;
				break;

			case 'a':
				g_auth = 1;
				break;

			case 'n':
				g_daemon = 0;
				break;

			case 's':
				g_syslog = 1;
				break;

			case 'v':
				g_verbose = 1;
				break;

			default:
				print_help();
				exit(EXIT_ARGS);
		}
	}

	if (g_daemon) {
		lprintf(LOG_DEBUG, "Daemonizing ...");
		if (-1 == daemon(0, 0)) {
			lprintf(LOG_ERR, "Failed daemonizing: %m");
			return 1;
		}
		openlog(__progname, LOG_CONS | LOG_PID, LOG_DAEMON);
	}

#ifdef HAVE_LIBCONFUSE
	if (!config) {
		snprintf(path, sizeof(path), "%s/%s.conf", CONFDIR, PACKAGE_NAME);
		config = path;
	} else if (access(config, F_OK)) {
		lprintf(LOG_ERR, "Failed reading config file '%s'\n", config);
		return 1;
	}

	if (read_config(config))
		return 1;
#endif

	if (!g_community)
		g_community = strdup("public");
	if (!g_vendor)
		g_vendor = strdup(VENDOR);
	if (!g_description)
		g_description = strdup("");
	if (!g_location)
		g_location = strdup("");
	if (!g_contact)
		g_contact = strdup("");

	/* Store the starting time since we need it for MIB updates */
	if (gettimeofday(&tv_last, NULL) == -1) {
		memset(&tv_last, 0, sizeof(tv_last));
		memset(&tv_sleep, 0, sizeof(tv_sleep));
	} else {
		tv_sleep.tv_sec = g_timeout / 100;
		tv_sleep.tv_usec = (g_timeout % 100) * 10000;
	}

	/* Build the MIB and execute the first MIB update to get actual values */
	if (mib_build() == -1)
		exit(EXIT_SYSCALL);
	if (mib_update(1) == -1)
		exit(EXIT_SYSCALL);

#ifdef DEBUG
	dump_mib(g_mib, g_mib_length);
#endif

	/* Open the server's UDP port and prepare it for listening */
	g_udp_sockfd = socket((g_family == AF_INET) ? PF_INET : PF_INET6, SOCK_DGRAM, 0);
	if (g_udp_sockfd == -1) {
		lprintf(LOG_ERR, "could not create UDP socket: %m\n");
		exit(EXIT_SYSCALL);
	}

	if (g_family == AF_INET) {
		sockaddr.sa.sin_family = g_family;
		sockaddr.sa.sin_port = htons(g_udp_port);
		sockaddr.sa.sin_addr = inaddr_any;
		socklen = sizeof(sockaddr.sa);
#ifdef CONFIG_ENABLE_IPV6
	} else {
		sockaddr.sa6.sin6_family = g_family;
		sockaddr.sa6.sin6_port = htons(g_udp_port);
		sockaddr.sa6.sin6_addr = in6addr_any;
		socklen = sizeof(sockaddr.sa6);
#endif
	}
	if (bind(g_udp_sockfd, (struct sockaddr *)&sockaddr, socklen) == -1) {
		lprintf(LOG_ERR, "could not bind UDP socket to port %d: %m\n", g_udp_port);
		exit(EXIT_SYSCALL);
	}

#ifndef __FreeBSD__
	if (g_bind_to_device) {
		snprintf(ifreq.ifr_ifrn.ifrn_name, sizeof(ifreq.ifr_ifrn.ifrn_name), "%s", g_bind_to_device);
		if (setsockopt(g_udp_sockfd, SOL_SOCKET, SO_BINDTODEVICE, (char *)&ifreq, sizeof(ifreq)) == -1) {
			lprintf(LOG_WARNING, "could not bind UDP socket to device %s: %m\n", g_bind_to_device);
			exit(EXIT_SYSCALL);
		}
	}
#endif

	/* Open the server's TCP port and prepare it for listening */
	g_tcp_sockfd = socket((g_family == AF_INET) ? PF_INET : PF_INET6, SOCK_STREAM, 0);
	if (g_tcp_sockfd == -1) {
		lprintf(LOG_ERR, "could not create TCP socket: %m\n");
		exit(EXIT_SYSCALL);
	}

#ifndef __FreeBSD__
	if (g_bind_to_device) {
		snprintf(ifreq.ifr_ifrn.ifrn_name, sizeof(ifreq.ifr_ifrn.ifrn_name), "%s", g_bind_to_device);
		if (setsockopt(g_tcp_sockfd, SOL_SOCKET, SO_BINDTODEVICE, (char *)&ifreq, sizeof(ifreq)) == -1) {
			lprintf(LOG_WARNING, "could not bind TCP socket to device %s: %m\n", g_bind_to_device);
			exit(EXIT_SYSCALL);
		}
	}
#endif

	c = 1;
	if (setsockopt(g_tcp_sockfd, SOL_SOCKET, SO_REUSEADDR, &c, sizeof(c)) == -1) {
		lprintf(LOG_WARNING, "could not set SO_REUSEADDR on TCP socket: %m\n");
		exit(EXIT_SYSCALL);
	}

	if (g_family == AF_INET) {
		sockaddr.sa.sin_family = g_family;
		sockaddr.sa.sin_port = htons(g_udp_port);
		sockaddr.sa.sin_addr = inaddr_any;
		socklen = sizeof(sockaddr.sa);
#ifdef CONFIG_ENABLE_IPV6
	} else {
		sockaddr.sa6.sin6_family = g_family;
		sockaddr.sa6.sin6_port = htons(g_udp_port);
		sockaddr.sa6.sin6_addr = in6addr_any;
		socklen = sizeof(sockaddr.sa6);
#endif
	}
	if (bind(g_tcp_sockfd, (struct sockaddr *)&sockaddr, socklen) == -1) {
		lprintf(LOG_ERR, "could not bind TCP socket to port %d: %m\n", g_tcp_port);
		exit(EXIT_SYSCALL);
	}

	if (listen(g_tcp_sockfd, 128) == -1) {
		lprintf(LOG_ERR, "could not prepare TCP socket for listening: %m\n");
		exit(EXIT_SYSCALL);
	}

	/* Print a starting message (so the user knows the args were ok) */
	if (g_bind_to_device) {
		lprintf(LOG_INFO, "Listening on port %d/udp and %d/tcp on interface %s\n",
			g_udp_port, g_tcp_port, g_bind_to_device);
	} else {
		lprintf(LOG_INFO, "Listening on port %d/udp and %d/tcp\n", g_udp_port, g_tcp_port);
	}

	/* Handle incoming connect requests and incoming data */
	while (!g_quit) {
		/* Sleep until we get a request or the timeout is over */
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(g_udp_sockfd, &rfds);
		FD_SET(g_tcp_sockfd, &rfds);
		nfds = (g_udp_sockfd > g_tcp_sockfd) ? g_udp_sockfd : g_tcp_sockfd;

		for (i = 0; i < g_tcp_client_list_length; i++) {
			if (g_tcp_client_list[i]->outgoing)
				FD_SET(g_tcp_client_list[i]->sockfd, &wfds);
			else
				FD_SET(g_tcp_client_list[i]->sockfd, &rfds);

			if (nfds < g_tcp_client_list[i]->sockfd)
				nfds = g_tcp_client_list[i]->sockfd;
		}

		if (select(nfds + 1, &rfds, &wfds, NULL, &tv_sleep) == -1) {
			if (g_quit)
				break;

			lprintf(LOG_ERR, "could not select from sockets: %m\n");
			exit(EXIT_SYSCALL);
		}

		/* Determine whether to update the MIB and the next ticks to sleep */
		ticks = ticks_since(&tv_last, &tv_now);
		if (ticks < 0 || ticks >= g_timeout) {
			lprintf(LOG_DEBUG, "updating the MIB (full)\n");
			if (mib_update(1) == -1)
				exit(EXIT_SYSCALL);

			memcpy(&tv_last, &tv_now, sizeof(tv_now));
			tv_sleep.tv_sec = g_timeout / 100;
			tv_sleep.tv_usec = (g_timeout % 100) * 10000;
		} else {
			lprintf(LOG_DEBUG, "updating the MIB (partial)\n");
			if (mib_update(0) == -1)
				exit(EXIT_SYSCALL);

			tv_sleep.tv_sec = (g_timeout - ticks) / 100;
			tv_sleep.tv_usec = ((g_timeout - ticks) % 100) * 10000;
		}

#ifdef DEBUG
		dump_mib(g_mib, g_mib_length);
#endif

		/* Handle UDP packets, TCP packets and TCP connection connects */
		if (FD_ISSET(g_udp_sockfd, &rfds))
			handle_udp_client();

		if (FD_ISSET(g_tcp_sockfd, &rfds))
			handle_tcp_connect();

		for (i = 0; i < g_tcp_client_list_length; i++) {
			if (g_tcp_client_list[i]->outgoing) {
				if (FD_ISSET(g_tcp_client_list[i]->sockfd, &wfds))
					handle_tcp_client_write(g_tcp_client_list[i]);
			} else {
				if (FD_ISSET(g_tcp_client_list[i]->sockfd, &rfds))
					handle_tcp_client_read(g_tcp_client_list[i]);
			}
		}

		/* If there was a TCP disconnect, remove the client from the list */
		for (i = 0; i < g_tcp_client_list_length; i++) {
			if (g_tcp_client_list[i]->sockfd == -1) {
				g_tcp_client_list_length--;
				if (i < g_tcp_client_list_length) {
					size_t len = (g_tcp_client_list_length - i) * sizeof(g_tcp_client_list[i]);

					free(g_tcp_client_list[i]);
					memmove(&g_tcp_client_list[i], &g_tcp_client_list[i + 1], len);
				}
			}
		}
	}

	/* We were killed, print a message and exit */
	lprintf(LOG_INFO, "stopped\n");

	return EXIT_OK;
}

/* vim: ts=4 sts=4 sw=4 nowrap
 */
