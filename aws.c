// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	struct stat st;

	fstat(conn->fd, &st);
	conn->file_size = st.st_size;
	sprintf(conn->send_buffer, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n"
								"Connection: close\r\n"
								"Content-Type: text/html\r\n\r\n", conn->file_size);

	conn->send_len = strlen(conn->send_buffer);
	conn->state = STATE_SENDING_DATA;
}

static void connection_prepare_send_404(struct connection *conn)
{
	struct stat st;

	fstat(conn->fd, &st);
	conn->file_size = st.st_size;
	sprintf(conn->send_buffer, "HTTP/1.1 404 Not Found\r\n"
								"Content-Length: %ld\r\n"
								"Connection: close\r\n"
								"\r\n", conn->file_size);

	conn->send_len = strlen(conn->send_buffer);
	conn->state = STATE_SENDING_DATA;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	if (strstr(conn->request_path, AWS_REL_STATIC_FOLDER))
		return RESOURCE_TYPE_STATIC;


	if (strstr(conn->request_path, AWS_REL_DYNAMIC_FOLDER))
		return RESOURCE_TYPE_DYNAMIC;


	return RESOURCE_TYPE_NONE;
}


struct connection *connection_create(int sockfd)
{
	struct connection *conn = calloc(1, sizeof(struct connection));

	if (!conn) {
		perror("calloc");
		return NULL;
	}


	conn->sockfd = sockfd;
	conn->fd = -1;

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	struct iocb *cb = malloc(sizeof(struct iocb));

	if (!cb) {
		perror("malloc");
		exit(1);
	}


	io_prep_pread(cb, conn->fd, conn->recv_buffer, BUFSIZ, conn->file_pos);


	int read_ret = io_submit(ctx, 1, &cb);

	if (read_ret != 1) {
		free(cb);
		perror("io_submit read");
		exit(1);
	}


	io_prep_pwrite(cb, conn->sockfd, conn->recv_buffer, BUFSIZ, 0);

	int write_ret = io_submit(ctx, 1, &cb);

	if (write_ret != 1) {
		free(cb);
		perror("io_submit write");
		exit(1);
	}

	free(cb);
}

void connection_remove(struct connection *conn)
{
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);

	if (conn->sockfd != -1)
		close(conn->sockfd);

	if (conn->fd != -1)
		close(conn->fd);

	free(conn);
	conn->state = STATE_CONNECTION_CLOSED;
}

void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	inet_ntoa(addr.sin_addr);

	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

	conn = connection_create(sockfd);

	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");

	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
}

void receive_data(struct connection *conn)
{
	conn->recv_len = 0;
	ssize_t bytes_received;

	do {
		bytes_received = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
							  BUFSIZ - conn->recv_len, 0);
		if (bytes_received == -1) {
			conn->state = STATE_REQUEST_RECEIVED;
			return;
		} else if (bytes_received == 0) {
			conn->state = STATE_REQUEST_RECEIVED;
			return;
		}

		conn->recv_len += bytes_received;
	} while (bytes_received > 0);

	conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	char filepath[BUFSIZ];

	memmove(filepath, AWS_DOCUMENT_ROOT, strlen(AWS_DOCUMENT_ROOT) + 1);
	strcat(filepath, conn->request_path + 1);

	int fd = open(filepath, O_RDONLY);

	if (fd < 0) {
		perror("open");
		conn->state = STATE_SENDING_404;
		return -1;
	}

	conn->fd = fd;
	conn->state = STATE_SENDING_HEADER;

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	struct io_event event;
	int read_ret = io_getevents(ctx, 1, 1, &event, NULL);

	if (read_ret != 1 || event.res < 0)
		return;

	conn->file_pos += event.res;

	int write_ret = io_getevents(ctx, 1, 1, &event, NULL);

	if (write_ret != 1 || event.res != BUFSIZ)
		return;

	conn->file_size -= event.res;

	if ((int) conn->file_size <= 0)
		conn->state = STATE_DATA_SENT;
	else
		conn->state = STATE_ASYNC_ONGOING;
}

int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	size_t parsed = http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	if (parsed != conn->recv_len) {
		conn->state = STATE_SENDING_404;
		return -1;
	}

	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	int rc;

	rc = sendfile(conn->sockfd, conn->fd, NULL, conn->file_size);
	if (rc == -1) {
		perror("sendfile");
		return -1;
	}

	conn->file_size -= rc;

	if (conn->file_size == 0) {
		conn->state = STATE_DATA_SENT;
		return STATE_DATA_SENT;
	}

	conn->state = STATE_SENDING_DATA;
	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	ssize_t bytes_sent;

	bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len, 0);

	if (bytes_sent < 0) {
		perror("send");
		return -1;
	}

	conn->send_len -= bytes_sent;
	conn->send_pos += bytes_sent;

	return bytes_sent;
}


int connection_send_dynamic(struct connection *conn)
{
	connection_start_async_io(conn);
	connection_complete_async_io(conn);

	return conn->state;
}


void handle_input(struct connection *conn)
{
	switch (conn->state) {
	case STATE_INITIAL:
		conn->state = STATE_RECEIVING_DATA;
		break;
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		break;
	default:
		break;
	}

	if (conn->state == STATE_REQUEST_RECEIVED) {
		w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);
	}
}

void handle_output(struct connection *conn)
{
	int bytes_sent;
	int rc;

	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		if (conn->recv_len <= 0) {
			perror("recv");
			connection_remove(conn);
			return;
		}

		rc = parse_header(conn);

		if (rc)
			break;

		conn->res_type = connection_get_resource_type(conn);
		rc = connection_open_file(conn);
		if (rc)
			break;

		break;

	case STATE_SENDING_404:
		connection_prepare_send_404(conn);
		break;
	case STATE_SENDING_HEADER:
		connection_prepare_send_reply_header(conn);
		break;
	case STATE_SENDING_DATA:
		if (conn->send_len > 0) {
			bytes_sent = connection_send_data(conn);

			if (bytes_sent > 0)
				break;
		}

		if (conn->fd != -1) {
			switch (conn->res_type) {
			case RESOURCE_TYPE_STATIC:
				bytes_sent += connection_send_static(conn);
				break;
			case RESOURCE_TYPE_DYNAMIC:
				conn->state = STATE_ASYNC_ONGOING;
				break;
			default:
				conn->state = STATE_DATA_SENT;
				break;
			}
		} else {
			conn->state = STATE_DATA_SENT;
		}
		break;

	case STATE_ASYNC_ONGOING:
		connection_start_async_io(conn);
		connection_complete_async_io(conn);
		break;

	case STATE_DATA_SENT:
		connection_remove(conn);
		break;
	default:
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	if (event & EPOLLIN)
		handle_input((struct connection *) conn);

	if (event & EPOLLOUT)
		handle_output((struct connection *) conn);
}

int main(void)
{
	int rc;

	rc = io_setup(128, &ctx);

	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	while (1) {
		struct epoll_event rev;

		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			handle_client(rev.events, (struct connection *) rev.data.ptr);
		}
	}

	return 0;
}
