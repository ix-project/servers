#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#include "config.h"
#include "common.h"
#include "memcached.h"

#define BUFSIZE 2048

struct conn {
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	volatile int lock;
#endif
	int fd;
	enum conn_state state;
	binary_header_t header;
	int buf_head;
	int buf_tail;
	unsigned char buf[BUFSIZE];
};

#define BACKLOG 8192
#define MAX_THREADS 64
#define EPOLLEXCLUSIVE (1 << 28)

static int epollfd[MAX_THREADS];
__thread int thread_no;
int nr_cpu;

static int avail_bytes(struct conn *conn)
{
	return conn->buf_tail - conn->buf_head;
}

static int recv_exactly(struct conn *conn, void *buf, size_t size)
{
	ssize_t ret;
	if (avail_bytes(conn) < size) {
		if (conn->buf_head) {
			memmove(conn->buf, &conn->buf[conn->buf_head], conn->buf_head);
			conn->buf_tail -= conn->buf_head;
			conn->buf_head = 0;
		}
		while (conn->buf_tail < size) {
			assert(BUFSIZE - conn->buf_tail > 0);
			ret = recv(conn->fd, &conn->buf[conn->buf_tail], BUFSIZE - conn->buf_tail, 0);
			if (ret <= 0)
				return ret;
			conn->buf_tail += ret;
		}
	}
	memcpy(buf, &conn->buf[conn->buf_head], size);
	conn->buf_head += size;

	return 1;
}

static int send_exactly(struct conn *conn, void *buf, size_t size)
{
	ssize_t ret;
	char *cbuf = (char *) buf;
	size_t partial = 0;

	while (partial < size) {
		ret = send(conn->fd, &cbuf[partial], size - partial, MSG_NOSIGNAL);
		if (ret <= 0)
			return ret;
		partial += ret;
	}

	return 1;
}

static int drain_exactly(struct conn *conn, size_t size)
{
	ssize_t ret;
	char buf[2048];
	size_t left;

	if (avail_bytes(conn) < size) {
		left = size;
		left -= avail_bytes(conn);
		conn->buf_head = 0;
		conn->buf_tail = 0;
		assert(left < sizeof(buf));
		while (left) {
			assert(left > 0);
			ret = recv(conn->fd, buf, left, 0);
			if (ret <= 0)
				return ret;
			left -= ret;
		}
	} else {
		conn->buf_head += size;
	}

	return 1;
}

static int handle_ret(struct conn *conn, ssize_t ret, int line)
{
	if (ret == 0) {
		close(conn->fd);
		/* TODO: should also free conn */
		return 1;
	} else if (ret == -1) {
		switch (errno) {
		case EAGAIN:
		case EBADF:
			return 1;
		case EPIPE:
		case ECONNRESET:
			close(conn->fd);
			/* TODO: should also free conn */
			return 1;
		default:
			fprintf(stderr, "Unexpected errno %d at line %d\n", errno, line);
		}
	}

	assert(ret == 1);

	return 0;
}

static void drive_machine(struct conn *conn)
{
	ssize_t ret;

	switch (conn->state) {
	case STATE_HEADER:
next_request:
		ret = recv_exactly(conn, &conn->header, sizeof(conn->header));
		if (handle_ret(conn, ret, __LINE__))
			return;
		assert(conn->header.magic == 0x80);
		conn->state = STATE_EXTRA;
		/* fallthrough */
	case STATE_EXTRA:
		ret = drain_exactly(conn, conn->header.extra_len);
		if (handle_ret(conn, ret, __LINE__))
			return;
		conn->state = STATE_KEY;
		/* fallthrough */
	case STATE_KEY:
		ret = drain_exactly(conn, __builtin_bswap16(conn->header.key_len));
		if (handle_ret(conn, ret, __LINE__))
			return;
		conn->state = STATE_VALUE;
		/* fallthrough */
	case STATE_VALUE:
		if (conn->header.opcode == CMD_SET) {
			ret = drain_exactly(conn, __builtin_bswap32(conn->header.body_len) - __builtin_bswap16(conn->header.key_len) - conn->header.extra_len);
			if (handle_ret(conn, ret, __LINE__))
				return;
		} else {
			assert(conn->header.opcode == CMD_GET);
		}
		conn->state = STATE_PROC;
		/* fallthrough */
	case STATE_PROC:
		process_request();
		conn->state = STATE_RESPONSE;
		conn->header.magic = 0x81;
		conn->header.status = __builtin_bswap16(1); /* Key not found */
		conn->header.body_len = 0;
		/* fallthrough */
	case STATE_RESPONSE:
		ret = send_exactly(conn, &conn->header, sizeof(conn->header));
		if (handle_ret(conn, ret, __LINE__))
			return;
		conn->state = STATE_HEADER;
		if (avail_bytes(conn) >= sizeof(conn->header))
			goto next_request;
		break;
	default:
		assert(0);
	}
}

static void epoll_ctl_add(int fd, void *arg)
{
	struct epoll_event ev;

	ev.events = EPOLLIN | EPOLLERR;
#if CONFIG_USE_EPOLLEXCLUSIVE
	ev.events |= EPOLLEXCLUSIVE;
#endif
	ev.data.fd = fd;
	ev.data.ptr = arg;
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	for (int i = 0; i < nr_cpu; i++) {
		if (epoll_ctl(epollfd[i], EPOLL_CTL_ADD, fd, &ev) == -1) {
			perror("epoll_ctl: EPOLL_CTL_ADD");
			exit(EXIT_FAILURE);
		}
	}
#else
	if (epoll_ctl(epollfd[thread_no], EPOLL_CTL_ADD, fd, &ev) == -1) {
		perror("epoll_ctl: EPOLL_CTL_ADD");
		exit(EXIT_FAILURE);
	}
#endif
}

static void setnonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	assert(flags >= 0);
	flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	assert(flags >= 0);
}

static int try_lock(struct conn *conn)
{
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	asm volatile("" : : : "memory");
	int ret = __sync_bool_compare_and_swap(&conn->lock, 0, 1);
	asm volatile("" : : : "memory");
	return ret;
#else
	return 1;
#endif
}

static void unlock(struct conn *conn)
{
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	asm volatile("" : : : "memory");
	conn->lock = 0;
	asm volatile("" : : : "memory");
#endif
}

static void *tcp_thread_main(void *arg)
{
	struct sockaddr_in sin;
	int sock;
	int one;
	int ret, i, nfds, conn_sock;
	struct epoll_event ev, events[CONFIG_MAX_EVENTS];
	struct conn *conn;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (!sock) {
		perror("socket");
		exit(1);
	}

	setnonblocking(sock);

	one = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *) &one, sizeof(one))) {
		perror("setsockopt(SO_REUSEPORT)");
		exit(1);
	}

	one = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *) &one, sizeof(one))) {
		perror("setsockopt(SO_REUSEADDR)");
		exit(1);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0);
	sin.sin_port = htons(11211);

	if (bind(sock, (struct sockaddr*)&sin, sizeof(sin))) {
		perror("bind");
		exit(1);
	}

	if (listen(sock, BACKLOG)) {
		perror("listen");
		exit(1);
	}

	thread_no = (long) arg;

	init_thread();

	epollfd[thread_no] = epoll_create1(0);
	ev.events = EPOLLIN;
	ev.data.u32 = 0;
	ret = epoll_ctl(epollfd[thread_no], EPOLL_CTL_ADD, sock, &ev);
	assert(!ret);

	while (1) {
		nfds = epoll_wait(epollfd[thread_no], events, CONFIG_MAX_EVENTS, -1);
		assert(nfds > 0);
		for (i = 0; i < nfds; i++) {
			if (events[i].data.u32 == 0) {
				conn_sock = accept(sock, NULL, NULL);
				if (conn_sock == -1) {
					perror("accept");
					exit(EXIT_FAILURE);
				}
				setnonblocking(conn_sock);
				if (setsockopt(conn_sock, IPPROTO_TCP, TCP_NODELAY, (void *) &one, sizeof(one))) {
					perror("setsockopt(TCP_NODELAY)");
					exit(1);
				}
				conn = malloc(sizeof *conn);
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
				conn->lock = 0;
#endif
				conn->fd = conn_sock;
				conn->state = STATE_HEADER;
				conn->buf_head = 0;
				conn->buf_tail = 0;
				epoll_ctl_add(conn_sock, conn);
			} else {
				conn = events[i].data.ptr;
				if (events[i].events & (EPOLLHUP | EPOLLERR)) {
					close(conn->fd);
					/* TODO: should also free conn */
				} else {
					if (try_lock(conn)) {
						drive_machine(conn);
						unlock(conn);
					}
				}
			}
		}
	}

	return NULL;
}

void init_linux(void)
{
	srand48(mytime());

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	nr_cpu = CPU_COUNT(&cpuset);
}

void start_linux_server(void)
{
	int i;
	pthread_t tid;

	for (i = 1; i < nr_cpu; i++) {
		if (pthread_create(&tid, NULL, tcp_thread_main, (void *) (long) i)) {
			fprintf(stderr, "failed to spawn thread %d\n", i);
			exit(-1);
		}
	}

	tcp_thread_main(0);
}
