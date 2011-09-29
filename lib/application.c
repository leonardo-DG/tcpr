#include <tcpr/application.h>

#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static const char state_path_format[] =
    "/var/tmp/tcpr-%s-%" PRId16 "-%" PRId16 ".state";
static const char control_path_format[] =
    "/var/tmp/tcpr-%s-%" PRId16 "-%" PRId16 ".ctl";

static struct tcpr *open_state(const char *peer_host, uint16_t peer_port,
			       uint16_t port, int flags)
{
	char path[sizeof(state_path_format) + strlen(peer_host) + 10];
	int fd;
	int open_flags;
	int saved_errno;
	struct tcpr *t;

	sprintf(path, state_path_format, peer_host, ntohs(peer_port),
		ntohs(port));

	open_flags = O_RDWR;
	if (flags & TCPR_CONNECTION_CREATE)
		open_flags |= O_CREAT;
	fd = open(path, open_flags, 0600);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, sizeof(*t)) < 0) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return NULL;
	}

	t = mmap(NULL, sizeof(*t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return t == MAP_FAILED ? NULL : t;
}

static void setup_control_address(struct sockaddr_un *control_address,
				  const char *peer_host, uint16_t peer_port,
				  uint16_t port)
{
	memset(control_address, 0, sizeof(*control_address));
	control_address->sun_family = AF_UNIX;
	sprintf(control_address->sun_path, control_path_format, peer_host,
		ntohs(peer_port), ntohs(port));
}

int tcpr_setup_connection(struct tcpr_connection *c, uint32_t peer_address,
			  uint16_t peer_port, uint16_t port, int flags)
{
	char host[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &peer_address, host, sizeof(host));

	c->control_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (c->control_socket < 0) {
		c->state = NULL;
		return -1;
	}

	setup_control_address(&c->control_address, host, peer_port, port);
	if (flags & TCPR_CONNECTION_FILTER) {
		unlink(c->control_address.sun_path);
		if (bind
		    (c->control_socket, (struct sockaddr *)&c->control_address,
		     sizeof(c->control_address)) < 0) {
			close(c->control_socket);
			c->control_socket = -1;
			c->state = NULL;
			return -1;
		}
	}

	c->state = open_state(host, peer_port, port, flags);
	if (!c->state) {
		if (flags & TCPR_CONNECTION_FILTER)
			unlink(c->control_address.sun_path);
		close(c->control_socket);
		c->control_socket = -1;
		return -1;
	}

	return 0;
}

void tcpr_destroy_connection(uint32_t peer_address, uint16_t peer_port,
			     uint16_t port)
{
	char host[INET_ADDRSTRLEN];
	char state[sizeof(state_path_format) + sizeof(host) + 10];
	char control[sizeof(control_path_format) + sizeof(host) + 10];

	inet_ntop(AF_INET, &peer_address, host, sizeof(host));
	sprintf(state, state_path_format, host, ntohs(peer_port), ntohs(port));
	sprintf(control, control_path_format, host, ntohs(peer_port),
		ntohs(port));
	unlink(state);
	unlink(control);
}

void tcpr_teardown_connection(struct tcpr_connection *c)
{
	if (c->state) {
		munmap(c->state, sizeof(*c->state));
		c->state = NULL;
	}
	if (c->control_socket >= 0) {
		close(c->control_socket);
		c->control_socket = -1;
	}
}

static int update(struct tcpr_connection *c)
{
	static const char message[] = "1\n";
	return sendto(c->control_socket, message, sizeof(message), 0,
		      (struct sockaddr *)&c->control_address,
		      sizeof(c->control_address));
}

size_t tcpr_output_bytes(struct tcpr_connection *c)
{
	return ntohl(c->state->peer.ack) - ntohl(c->state->saved.safe);
}

size_t tcpr_input_bytes(struct tcpr_connection *c)
{
	return ntohl(c->state->ack) - ntohl(c->state->saved.ack);
}

void tcpr_checkpoint_output(struct tcpr_connection *c, size_t bytes)
{
	c->state->saved.safe = htonl(ntohl(c->state->saved.safe) + bytes);
}

int tcpr_checkpoint_input(struct tcpr_connection *c, size_t bytes)
{
	c->state->saved.ack = htonl(ntohl(c->state->saved.ack) + bytes);
	return update(c);
}

void tcpr_shutdown_output(struct tcpr_connection *c)
{
	c->state->saved.done_writing = 1;
}

int tcpr_shutdown_input(struct tcpr_connection *c)
{
	c->state->saved.done_reading = 1;
	return update(c);
}

int tcpr_close(struct tcpr_connection *c)
{
	tcpr_shutdown_output(c);
	return tcpr_shutdown_input(c);
}

void tcpr_wait(struct tcpr_connection *c)
{
	while (!c->state->done)
		sleep(1);
}
