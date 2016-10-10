/*************************************************************************
  Copyright (c) 2015 VAYAVYA LABS PVT LTD - http://vayavyalabs.com/
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.

   3. Neither the name of the Vayavya labs nor the names of its
	  contributors may be used to endorse or promote products derived from
	  this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if.h>

#include <netinet/in.h>

#include <arpa/inet.h>

#include "maap.h"
#include "maap_packet.h"
#include "maap_parse.h"

#define MAX_CLIENT_CONNECTIONS 32
#define DEFAULT_PORT           "15364"

#define VERSION_STR	"0.1"

static int init_maap_networking(const char *iface, uint8_t src_mac[ETH_ALEN], uint8_t dest_mac[ETH_ALEN]);
static int get_listener_socket(const char *listenport);
static int act_as_client(const char *listenport);


static const char *version_str =
	"maap_daemon v" VERSION_STR "\n"
	"Copyright (c) 2014-2015, VAYAVYA LABS PVT LTD\n"
	"Copyright (c) 2016, Harman International Industries Inc.\n";

static void usage(void)
{
	fprintf(stderr,
		"\n"
		"usage: maap_daemon [-c | [-d] -i interface-name] [-p port_num]"
		"\n"
		"options:\n"
		"\t-c  Run as a client (sends commands to the daemon)\n"
		"\t-d  Run daemon in the background\n"
		"\t-i  Specify daemon interface to monitor\n"
		"\t-p  Specify the control port to connect to (client) or listen to (daemon).\n"
		"\t    The default port is " DEFAULT_PORT ".\n"
		"\n" "%s" "\n", version_str);
	exit(1);
}

/* get sockaddr, IPv4 or IPv6 */
static void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


int main(int argc, char *argv[])
{
	Maap_Client mc;

	int c;
	int as_client = 0, daemonize = 0;
	char *iface = NULL;
	char *listenport = NULL;
	int ret;

	int socketfd;
	uint8_t dest_mac[ETH_ALEN] = MAAP_DEST_MAC;
	uint8_t src_mac[ETH_ALEN];

	int listener;

	int clientfd[MAX_CLIENT_CONNECTIONS];
	int i, nextclientindex;

	fd_set master, read_fds;
	int fdmax;

	void *packet_data;
	int64_t waittime;
	struct timeval tv;
	char recvbuffer[1600];
	int recvbytes;
	Maap_Cmd recvcmd;
	Maap_Notify recvnotify;
	uintptr_t notifysocket;
	int exit_received = 0;


	/*
	 *  Parse the arguments
	 */

	while ((c = getopt(argc, argv, "hcdi:p:")) >= 0)
	{
		switch (c)
		{
		case 'c':
			as_client = 1;
			break;

		case 'd':
			daemonize = 1;
			break;

		case 'i':
			if (iface)
			{
				fprintf(stderr, "Only one interface per daemon is supported\n");
				usage();
			}
			iface = strdup(optarg);
			break;

		case 'p':
			if (listenport)
			{
				fprintf(stderr, "Only one port per daemon is supported\n");
				usage();
			}
			listenport = strdup(optarg);
			break;

		case 'h':
		default:
			usage();
			break;
		}
	}
	if (optind < argc)
	{
		usage();
	}

	if (as_client && daemonize)
	{
		fprintf(stderr, "Cannot run as both a client and a daemon\n");
		usage();
	}

	if (!as_client && iface == NULL)
	{
		fprintf(stderr, "A network interface is required as a daemon\n");
		usage();
	}
	if (as_client && iface != NULL)
	{
		fprintf(stderr, "A network interface is not supported as a client\n");
		usage();
	}

	if (daemonize) {
		ret = daemon(1, 0);
		if (ret) {
			fprintf(stderr, "Error: Failed to daemonize\n");
			return -1;
		}
	}

	if (listenport == NULL)
	{
		/* Use the default port. */
		listenport = strdup(DEFAULT_PORT);
	}

	if (as_client)
	{
		/* Run as a client instead of a server. */
		ret = act_as_client(listenport);
		free(listenport);
		return ret;
	}


	/*
	 * Initialize the networking support.
	 */

	socketfd = init_maap_networking(iface, src_mac, dest_mac);
	if (socketfd == -1) {
		return -1;
	}

	free(iface);
	iface = NULL;

	FD_ZERO(&read_fds);
	FD_ZERO(&master);
	FD_SET(STDIN_FILENO, &master);
	FD_SET(socketfd, &master);
	fdmax = socketfd;


	/*
	 * Initialize the client connection listen socket.
	 */

	listener = get_listener_socket(listenport);
	if (listener == -1) {
		close(socketfd);
		return -1;
	}

	free(listenport);
	listenport = NULL;

	/* Add the listener to the master set */
	FD_SET(listener, &master);

	/* Keep track of the biggest file descriptor */
	if (listener > fdmax) {
		fdmax = listener;
	}

	for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i) { clientfd[i] = -1; }
	nextclientindex = 0;


	/*
	 * Initialize the Maap_Client data structure.
	 */

	memset(&mc, 0, sizeof(mc));
	mc.dest_mac = convert_mac_address(dest_mac);
	mc.src_mac = convert_mac_address(src_mac);


	/*
	 * Seed the random number generator.
	 * This seeding is defined in IEEE 1722-2011 B.3.6.1.
	 */

	srand((unsigned int)mc.src_mac + (unsigned int)time(NULL));


	/*
	 * Main event loop
	 */

	while (!exit_received)
	{
		/* Send any queued packets. */
		while (mc.net != NULL && (packet_data = Net_getNextQueuedPacket(mc.net)) != NULL)
		{
			if (send(socketfd, packet_data, MAAP_NET_BUFFER_SIZE, 0) < 0)
			{
				/* Something went wrong.  Abort! */
				fprintf(stderr, "Error %d writing to network socket (%s)\n", errno, strerror(errno));
				Net_freeQueuedPacket(mc.net, packet_data);
				break;
			}
			Net_freeQueuedPacket(mc.net, packet_data);
		}

		/* Process any notifications. */
		while (get_notify(&mc, (void *)&notifysocket, &recvnotify) > 0)
		{
			print_notify(&recvnotify);

			if ((int) notifysocket != -1)
			{
				/* Send the notification information to the client. */
				for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i)
				{
					if (clientfd[i] == (int) notifysocket)
					{
						if (send((int) notifysocket, &recvnotify, sizeof(recvnotify), 0) < 0)
						{
							/* Something went wrong. Assume the socket will be closed below. */
							fprintf(stderr, "Error %d writing to client socket %d (%s)\n", errno, (int) notifysocket, strerror(errno));
						}
						break;
					}
				}
				if (i >= MAX_CLIENT_CONNECTIONS)
				{
					printf("Notification for client socket %d, but that socket no longer exists\n", (int) notifysocket);
				}
			}
		}

		/* Determine how long to wait. */
		waittime = maap_get_delay_to_next_timer(&mc);
		if (waittime > 0)
		{
			tv.tv_sec = waittime / 1000000000;
			tv.tv_usec = (waittime % 1000000000) / 1000;
		}
		else
		{
			/* Act immediately. */
			tv.tv_sec = 0;
			tv.tv_usec = 0;
		}

		/* Wait for something to happen. */
		read_fds = master;
		ret = select(fdmax+1, &read_fds, NULL, NULL, &tv);
		if (ret < 0)
		{
			fprintf(stderr, "select() error %d (%s)\n", errno, strerror(errno));
			break;
		}
		if (ret == 0)
		{
			/* The timer timed out.  Handle the timer. */
			maap_handle_timer(&mc);
			continue;
		}

		/* Handle any packets received. */
		if (FD_ISSET(socketfd, &read_fds))
		{
			struct sockaddr_ll ll_addr;
			socklen_t addr_len;

			while ((recvbytes = recvfrom(socketfd, recvbuffer, sizeof(recvbuffer), MSG_DONTWAIT, (struct sockaddr*)&ll_addr, &addr_len)) > 0)
			{
				maap_handle_packet(&mc, (uint8_t *)recvbuffer, recvbytes);
			}
			if (recvbytes < 0 && errno != EWOULDBLOCK)
			{
				/* Something went wrong.  Abort! */
				fprintf(stderr, "Error %d reading from network socket (%s)\n", errno, strerror(errno));
				break;
			}
		}

		/* Accept any new connections. */
		if (FD_ISSET(listener, &read_fds)) {
			int newfd;
			socklen_t addrlen;
			struct sockaddr_storage remoteaddr;
			char remoteIP[INET6_ADDRSTRLEN];

			addrlen = sizeof remoteaddr;
			newfd = accept(listener,
				(struct sockaddr *)&remoteaddr,
				&addrlen);

			if (newfd == -1) {
				fprintf(stderr, "Error %d accepting connection (%s)\n", errno, strerror(errno));
			} else {
				printf("New connection from %s on socket %d\n",
					inet_ntop(remoteaddr.ss_family,
						get_in_addr((struct sockaddr*)&remoteaddr),
						remoteIP, INET6_ADDRSTRLEN),
					newfd);

				/* Add the socket to our array of connected sockets. */
				if (clientfd[nextclientindex] != -1)
				{
					/* Find the next available index. */
					for (i = (nextclientindex + 1) % MAX_CLIENT_CONNECTIONS; i != nextclientindex; i = (i + 1) % MAX_CLIENT_CONNECTIONS)
					{
						if (clientfd[nextclientindex] == -1)
						{
							/* Found an empty array slot. */
							break;
						}
					}
					if (i == nextclientindex)
					{
						/* No more client slots available.  Connection rejected. */
						fprintf(stderr, "Out of client connection slots.  Connection rejected.\n");
						close(newfd);
						newfd = -1;
					}
				}

				if (newfd != -1)
				{
					clientfd[nextclientindex] = newfd;
					nextclientindex = (nextclientindex + 1) % MAX_CLIENT_CONNECTIONS; /* Next slot used for the next try. */
					FD_SET(newfd, &master); /* add to master set */
					if (newfd > fdmax) {    /* keep track of the max */
						fdmax = newfd;
					}
				}
			}
		}

		/* Handle any commands received via stdin. */
		if (FD_ISSET(STDIN_FILENO, &read_fds))
		{
			recvbytes = read(STDIN_FILENO, recvbuffer, sizeof(recvbuffer) - 1);
			if (recvbytes <= 0)
			{
				fprintf(stderr, "Error %d reading from stdin (%s)\n", errno, strerror(errno));
			}
			else
			{
				recvbuffer[recvbytes] = '\0';

				/* Process the command data (may be binary or text). */
				memset(&recvcmd, 0, sizeof(recvcmd));
				if (parse_write(&mc, (const void *)(uintptr_t) -1, recvbuffer))
				{
					/* Received a command to exit. */
					exit_received = 1;
				}
			}
		}

		/* Run through the existing connections looking for data to read. */
		for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i)
		{
			if (clientfd[i] != -1 && FD_ISSET(clientfd[i], &read_fds))
			{
				recvbytes = recv(clientfd[i], recvbuffer, sizeof(recvbuffer), 0);
				if (recvbytes < 0)
				{
					fprintf(stderr, "Error %d reading from socket %d (%s).  Connection closed.\n", errno, clientfd[i], strerror(errno));
					close(clientfd[i]);
					FD_CLR(clientfd[i], &master); /* remove from master set */
					clientfd[i] = -1;
					nextclientindex = i; /* We know this slot will be empty. */
				}
				else if (recvbytes == 0)
				{
					printf("Socket %d closed\n", clientfd[i]);
					close(clientfd[i]);
					FD_CLR(clientfd[i], &master); /* remove from master set */
					clientfd[i] = -1;
					nextclientindex = i; /* We know this slot will be empty. */
				}
				else
				{
					recvbuffer[recvbytes] = '\0';

					/* Process the command data (may be binary or text). */
					memset(&recvcmd, 0, sizeof(recvcmd));
					if (parse_write(&mc, (const void *)(uintptr_t) clientfd[i], recvbuffer))
					{
						/* Received a command to exit. */
						exit_received = 1;
					}
				}
			}
		}

	}

	close(socketfd);
	close(listener);

	/* Close any connected sockets. */
	for (i = 0; i < MAX_CLIENT_CONNECTIONS; ++i) {
		if (clientfd[i] != -1) {
			close(clientfd[i]);
			clientfd[i] = -1;
		}
	}

	maap_deinit_client(&mc);

	return (exit_received ? 0 : -1);
}

/* Initializes the MAAP raw socket support, and returns a socket handle for that socket. */
static int init_maap_networking(const char *iface, uint8_t src_mac[ETH_ALEN], uint8_t dest_mac[ETH_ALEN])
{
	int socketfd;
	struct ifreq ifbuffer;
	int ifindex;
	struct sockaddr_ll sockaddr;
	struct packet_mreq mreq;

	if ((socketfd = socket(PF_PACKET, SOCK_RAW, htons(MAAP_TYPE))) == -1 )
	{
		fprintf(stderr, "Error: could not open socket %d\n",socketfd);
		return -1;
	}

	if (fcntl(socketfd, F_SETFL, O_NONBLOCK) < 0)
	{
		fprintf(stderr, "Error: could not set the socket to non-blocking\n");
		close(socketfd);
		return -1;
	}

	memset(&ifbuffer, 0x00, sizeof(ifbuffer));
	strncpy(ifbuffer.ifr_name, iface, IFNAMSIZ);
	if (ioctl(socketfd, SIOCGIFINDEX, &ifbuffer) < 0)
	{
		fprintf(stderr, "Error: could not get interface index\n");
		close(socketfd);
		return -1;
	}

	ifindex = ifbuffer.ifr_ifindex;
	if (ioctl(socketfd, SIOCGIFHWADDR, &ifbuffer) < 0) {
		fprintf(stderr, "Error: could not get interface address\n");
		close(socketfd);
		return -1;
	}

	memcpy(src_mac, ifbuffer.ifr_hwaddr.sa_data, ETH_ALEN);

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.sll_family = AF_PACKET;
	sockaddr.sll_ifindex = ifindex;
	sockaddr.sll_halen = ETH_ALEN;
	memcpy(sockaddr.sll_addr, dest_mac, ETH_ALEN);

	if (bind(socketfd, (struct sockaddr*)&sockaddr, sizeof(sockaddr))) {
		fprintf(stderr, "Error: could not bind datagram socket\n");
		close(socketfd);
		return -1;
	}

	/* filter multicast address */
	memset(&mreq, 0, sizeof(mreq));
	mreq.mr_ifindex = ifindex;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = 6;
	memcpy(mreq.mr_address, dest_mac, mreq.mr_alen);

	if (setsockopt(socketfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq,
		sizeof(mreq)) < 0) {
		fprintf(stderr, "setsockopt PACKET_ADD_MEMBERSHIP failed\n");
		close(socketfd);
		return -1;
	}

	return socketfd;
}

/* Initializes the listener socket, and returns a socket handle for that socket. */
static int get_listener_socket(const char *listenport)
{
	int listener;
	struct addrinfo hints, *ai, *p;
	int yes=1;
	int ret;

	/* Get us a localhost socket and bind it */
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if ((ret = getaddrinfo(NULL, listenport, &hints, &ai)) != 0) {
		fprintf(stderr, "getaddrinfo failure %s\n", gai_strerror(ret));
		return -1;
	}

	for(p = ai; p != NULL; p = p->ai_next) {
		listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (listener == -1) {
			continue;
		}

		/* Lose the pesky "address already in use" error message */
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

		if (bind(listener, p->ai_addr, p->ai_addrlen) < 0) {
			freeaddrinfo(ai);
			close(listener);
			continue;
		}

		break;
	}

	freeaddrinfo(ai);

	/* If we got here, it means we didn't get bound */
	if (p == NULL) {
		fprintf(stderr, "Socket failed to bind error %d (%s)\n", errno, strerror(errno));
		close(listener);
		return -1;
	}

	if (listen(listener, 10) < 0) {
		fprintf(stderr, "Socket listen error %d (%s)\n", errno, strerror(errno));
		close(listener);
		return -1;
	}

	return listener;
}


/* Local function to handle client side of network command & control. */
static int act_as_client(const char *listenport)
{
	int socketfd;
	struct addrinfo hints, *ai, *p;
	int ret;

	fd_set master, read_fds;
	int fdmax;

	char recvbuffer[200];
	int recvbytes;
	Maap_Cmd recvcmd;
	int exit_received = 0;

	/* Create a localhost socket. */
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	if ((ret = getaddrinfo("localhost", listenport, &hints, &ai)) != 0) {
		fprintf(stderr, "getaddrinfo failure %s\n", gai_strerror(ret));
		return -1;
	}

	for(p = ai; p != NULL; p = p->ai_next) {
		socketfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (socketfd != -1) {
			break;
		}
	}

	if (p == NULL) {
		fprintf(stderr, "Socket creation error %d (%s)\n", errno, strerror(errno));
		freeaddrinfo(ai);
		return -1;
	}

	/* Connect to the MAAP daemon. */
	if (connect(socketfd, p->ai_addr, p->ai_addrlen) < 0)
	{
		fprintf(stderr, "Unable to connect to the daemon, error %d (%s)\n", errno, strerror(errno));
		freeaddrinfo(ai);
		close(socketfd);
		return -1;
	}

	freeaddrinfo(ai);

	if (fcntl(socketfd, F_SETFL, O_NONBLOCK) < 0)
	{
		fprintf(stderr, "Error: could not set the socket to non-blocking\n");
		close(socketfd);
		return -1;
	}

	FD_ZERO(&read_fds);
	FD_ZERO(&master);
	FD_SET(STDIN_FILENO, &master);
	FD_SET(socketfd, &master);
	fdmax = socketfd;


	/*
	 * Main event loop
	 */

	while (!exit_received)
	{
		/* Wait for something to happen. */
		read_fds = master;
		ret = select(fdmax+1, &read_fds, NULL, NULL, NULL);
		if (ret <= 0)
		{
			fprintf(stderr, "select() error %d (%s)\n", errno, strerror(errno));
			break;
		}

		/* Handle any responses received. */
		if (FD_ISSET(socketfd, &read_fds))
		{
			while ((recvbytes = recv(socketfd, recvbuffer, sizeof(recvbuffer) - 1, 0)) > 0)
			{
				recvbuffer[recvbytes] = '\0';

				/* Process the response data (will be binary). */
				if (recvbytes == sizeof(Maap_Notify))
				{
					print_notify((Maap_Notify *) recvbuffer);
				}
				else
				{
					fprintf(stderr, "Received unexpected response of size %d\n", recvbytes);
				}
			}
			if (recvbytes == 0)
			{
				/* The MAAP daemon closed the connection.  Assume it shut down, and we should too. */
				printf("MAAP daemon exited.  Closing application.\n");
				exit_received = 1;
			}
			if (recvbytes < 0 && errno != EWOULDBLOCK)
			{
				/* Something went wrong.  Abort! */
				fprintf(stderr, "Error %d reading from network socket (%s)\n", errno, strerror(errno));
				break;
			}
		}

		/* Handle any commands received via stdin. */
		if (FD_ISSET(STDIN_FILENO, &read_fds))
		{
			recvbytes = read(STDIN_FILENO, recvbuffer, sizeof(recvbuffer) - 1);
			if (recvbytes <= 0)
			{
				fprintf(stderr, "Error %d reading from stdin (%s)\n", errno, strerror(errno));
			}
			else
			{
				Maap_Cmd *bufcmd = (Maap_Cmd *) recvbuffer;
				int rv = 0;

				recvbuffer[recvbytes] = '\0';

				/* Determine the command requested (may be binary or text). */
				switch (bufcmd->kind) {
				case MAAP_CMD_INIT:
				case MAAP_CMD_RESERVE:
				case MAAP_CMD_RELEASE:
				case MAAP_CMD_STATUS:
				case MAAP_CMD_EXIT:
					memcpy(&recvcmd, bufcmd, sizeof(Maap_Cmd));
					rv = 1;
					break;
				default:
					memset(&recvcmd, 0, sizeof(Maap_Cmd));
					rv = parse_text_cmd(recvbuffer, &recvcmd);
					break;
				}

				/* If the command is valid, Send it to the MAAP daemon. */
				if (rv)
				{
					if (send(socketfd, (char *) &recvcmd, sizeof(Maap_Cmd), 0) < 0)
					{
						/* Something went wrong.  Abort! */
						fprintf(stderr, "Error %d writing to network socket (%s)\n", errno, strerror(errno));
						break;
					}
				}
			}
		}

	}

	close(socketfd);

	return (exit_received ? 0 : -1);
}
