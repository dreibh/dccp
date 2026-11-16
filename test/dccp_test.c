#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SOCK_DCCP
#define SOCK_DCCP 6
#endif
#ifndef IPPROTO_DCCP
#define IPPROTO_DCCP 33
#endif
#ifndef SOL_DCCP
#define SOL_DCCP 269
#endif
#ifndef DCCP_SOCKOPT_SERVICE
#define DCCP_SOCKOPT_SERVICE 2
#endif
#ifndef DCCP_SOCKOPT_GET_CUR_MPS
#define DCCP_SOCKOPT_GET_CUR_MPS 5
#endif

static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

static int make_dccp_socket(int family, uint32_t service_code)
{
	int fd = socket(family, SOCK_DCCP, IPPROTO_DCCP);
	if (fd < 0)
		die("socket(SOCK_DCCP)");

	uint32_t svc = htonl(service_code);
	if (setsockopt(fd, SOL_DCCP, DCCP_SOCKOPT_SERVICE, &svc, sizeof(svc)) < 0)
		die("setsockopt(DCCP_SOCKOPT_SERVICE)");

	return fd;
}

static void run_server(int family, const char *bind_ip, uint16_t port, uint32_t svc)
{
	int lfd = make_dccp_socket(family, svc);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	if (family == AF_INET6) {
		struct sockaddr_in6 addr6 = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(port),
		};
		if (inet_pton(AF_INET6, bind_ip, &addr6.sin6_addr) != 1)
			die("inet_pton(AF_INET6)");
		if (bind(lfd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0)
			die("bind(AF_INET6)");
	} else {
		struct sockaddr_in addr4 = {
			.sin_family = AF_INET,
			.sin_port = htons(port),
		};
		if (inet_pton(AF_INET, bind_ip, &addr4.sin_addr) != 1)
			die("inet_pton(AF_INET)");
		if (bind(lfd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0)
			die("bind(AF_INET)");
	}

	if (listen(lfd, 5) < 0)
		die("listen");

	printf("[server] listening on [%s]:%u (service=%u)\n", bind_ip, port, svc);
	fflush(stdout);

	int cfd = accept(lfd, NULL, NULL);
	if (cfd < 0)
		die("accept");

	uint32_t mps = 0;
	socklen_t optlen = sizeof(mps);
	if (getsockopt(cfd, SOL_DCCP, DCCP_SOCKOPT_GET_CUR_MPS, &mps, &optlen) == 0)
		printf("[server] accepted connection (MPS=%u bytes)\n", mps);

	char buf[512];
	ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
	if (n < 0)
		die("recv");
	buf[n] = '\0';
	printf("[server] received (%zd bytes): \"%s\"\n", n, buf);

	const char *reply = "pong over DCCP";
	if (send(cfd, reply, strlen(reply), 0) < 0)
		die("send");

	/* Active close from server exercises DCCP_CLOSING -> DCCP_TIME_WAIT */
	close(cfd);
	close(lfd);
}

static void run_client(int family, const char *dst_ip, uint16_t port, uint32_t svc)
{
	int fd = make_dccp_socket(family, svc);

	if (family == AF_INET6) {
		struct sockaddr_in6 addr6 = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(port),
		};
		if (inet_pton(AF_INET6, dst_ip, &addr6.sin6_addr) != 1)
			die("inet_pton(AF_INET6)");
		if (connect(fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0)
			die("connect(AF_INET6)");
	} else {
		struct sockaddr_in addr4 = {
			.sin_family = AF_INET,
			.sin_port = htons(port),
		};
		if (inet_pton(AF_INET, dst_ip, &addr4.sin_addr) != 1)
			die("inet_pton(AF_INET)");
		if (connect(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0)
			die("connect(AF_INET)");
	}

	const char *msg = "ping over DCCP";
	if (send(fd, msg, strlen(msg), 0) < 0)
		die("send");
	printf("[client] sent: \"%s\"\n", msg);

	char buf[512];
	ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
	if (n < 0)
		die("recv");
	buf[n] = '\0';
	printf("[client] received (%zd bytes): \"%s\"\n", n, buf);

	close(fd);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "Usage: %s <server|client> <4|6> <ip> [port]\n", argv[0]);
		return EXIT_FAILURE;
	}

	int family = (strcmp(argv[2], "6") == 0) ? AF_INET6 : AF_INET;
	const char *ip = argv[3];
	uint16_t port = (argc >= 5) ? (uint16_t)atoi(argv[4]) : 5000;
	uint32_t service_code = 12345;

	if (strcmp(argv[1], "server") == 0)
		run_server(family, ip, port, service_code);
	else
		run_client(family, ip, port, service_code);

	return 0;
}
