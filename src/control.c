#include <tcpr/application.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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

struct state {
	const char *peer_host;
	const char *peer_port;
	const char *port;
	const char *save_file;
	const char *recovery_file;
	int input_bytes;
	int output_bytes;
	int wait;
	int destroy;
	struct sockaddr_in peer_address;
	struct sockaddr_in address;
	struct tcpr_connection tcpr;
};

static void print_help_and_exit(const char *program)
{
	fprintf(stderr, "Usage: %s [OPTIONS]\n", program);
	fprintf(stderr, "\n");
	fprintf(stderr, "Manipulate and display TCPR connection state.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -a PORT  The application is bound to PORT.\n");
	fprintf(stderr, "  -h HOST  The peer is bound to HOST.\n");
	fprintf(stderr, "  -p PORT  The peer is bound to PORT.\n");
	fprintf(stderr, "  -S FILE  Save the connection state into FILE.\n");
	fprintf(stderr, "  -R FILE  Recover the connection state from FILE.\n");
	fprintf(stderr, "  -I NUM   Acknowledge NUM bytes of input.\n");
	fprintf(stderr, "  -I all   Acknowledge all outstanding input.\n");
	fprintf(stderr, "  -I done  Shut down input.\n");
	fprintf(stderr, "  -O NUM   Checkpoint NUM bytes of output.\n");
	fprintf(stderr, "  -O all   Checkpoint all outstanding output.\n");
	fprintf(stderr, "  -O done  Shut down output.\n");
	fprintf(stderr, "  -W       Wait until the connection is done.\n");
	fprintf(stderr, "  -D       Destroy the connection state.\n");
	fprintf(stderr, "  -?       Print this help message and exit.\n");
	exit(EXIT_FAILURE);
}

static void handle_options(struct state *s, int argc, char **argv)
{
	int o;

	s->peer_host = "127.0.0.1";
	s->peer_port = "9999";
	s->port = "8888";
	s->save_file = NULL;
	s->recovery_file = NULL;
	s->input_bytes = 0;
	s->output_bytes = 0;
	s->wait = 0;
	s->destroy = 0;

	while ((o = getopt(argc, argv, "a:h:p:S:R:I:O:WD?")) != -1)
		switch (o) {
		case 'a':
			s->port = optarg;
			break;
		case 'h':
			s->peer_host = optarg;
			break;
		case 'p':
			s->peer_port = optarg;
			break;
		case 'S':
			s->save_file = optarg;
			break;
		case 'R':
			s->recovery_file = optarg;
			break;
		case 'I':
			if (!strcmp(optarg, "done"))
				s->input_bytes = -1;
			else if (!strcmp(optarg, "all"))
				s->input_bytes = INT_MAX;
			else
				s->input_bytes = atoi(optarg);
			break;
		case 'O':
			if (!strcmp(optarg, "done"))
				s->output_bytes = -1;
			else if (!strcmp(optarg, "all"))
				s->output_bytes = INT_MAX;
			else
				s->output_bytes = atoi(optarg);
			break;
		case 'W':
			s->wait = 1;
			break;
		case 'D':
			s->destroy = 1;
			break;
		default:
			print_help_and_exit(argv[0]);
		}
}

static void setup_state(struct state *s)
{
	int err;
	int flags = 0;
	struct addrinfo *ai;
	struct addrinfo *peer_ai;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	err = getaddrinfo(s->peer_host, s->peer_port, &hints, &peer_ai);
	if (err) {
		fprintf(stderr, "Resolving peer: %s\n", gai_strerror(err));
		exit(EXIT_FAILURE);
	}
	memcpy(&s->peer_address, peer_ai->ai_addr, sizeof(s->peer_address));
	freeaddrinfo(peer_ai);

	err = getaddrinfo(NULL, s->port, &hints, &ai);
	if (err) {
		fprintf(stderr, "Resolving port: %s\n", gai_strerror(err));
		exit(EXIT_FAILURE);
	}
	memcpy(&s->address, ai->ai_addr, sizeof(s->address));
	freeaddrinfo(ai);

	if (s->recovery_file)
		flags |= TCPR_CONNECTION_CREATE;
	if (tcpr_setup_connection
	    (&s->tcpr, &s->peer_address, s->address.sin_port, flags) < 0) {
		perror("Opening state");
		exit(EXIT_FAILURE);
	}
}

static void update_state(struct state *s)
{
	int fd;
	size_t bytes;

	if (s->recovery_file) {
		fd = open(s->recovery_file, O_RDONLY);
		if (fd < 0) {
			perror("Opening recovery file");
			exit(EXIT_FAILURE);
		}
		if (read
		    (fd, &s->tcpr.state->saved,
		     sizeof(s->tcpr.state->saved)) !=
		    sizeof(s->tcpr.state->saved)) {
			perror("Recovering");
			exit(EXIT_FAILURE);
		}
		if (close(fd) < 0) {
			perror("Closing recovery file");
			exit(EXIT_FAILURE);
		}
	}

	if (s->output_bytes < 0) {
		tcpr_shutdown_output(&s->tcpr);
	} else if (s->output_bytes > 0) {
		bytes = tcpr_output_bytes(&s->tcpr);
		if ((size_t)s->output_bytes < bytes)
			bytes = s->output_bytes;
		tcpr_checkpoint_output(&s->tcpr, bytes);
	}

	if (s->input_bytes < 0) {
		tcpr_shutdown_input(&s->tcpr);
	} else if (s->input_bytes > 0) {
		bytes = tcpr_input_bytes(&s->tcpr);
		if ((size_t)s->input_bytes < bytes)
			bytes = s->input_bytes;
		tcpr_checkpoint_input(&s->tcpr, bytes);
	}
}

static void print_state(struct state *s)
{
	printf("saved ACK\t%" PRIu32 "\n", ntohl(s->tcpr.state->saved.ack));
	printf("saved peer ACK\t%" PRIu32 "\n",
	       ntohl(s->tcpr.state->saved.safe));
	if (s->tcpr.state->saved.peer.mss)
		printf("peer MSS\t%" PRIu16 "\n",
		       s->tcpr.state->saved.peer.mss);
	if (s->tcpr.state->saved.peer.ws)
		printf("peer WS\t%" PRIu8 "\n",
		       s->tcpr.state->saved.peer.ws - 1);
	if (s->tcpr.state->saved.peer.sack_permitted)
		printf("peer SACK permitted\n");
	printf("delta\t%" PRIu32 "\n", s->tcpr.state->delta);
	printf("ACK\t%" PRIu32 "\n", ntohl(s->tcpr.state->ack));
	if (s->tcpr.state->have_fin)
		printf("FIN\t%" PRIu32 "\n", ntohl(s->tcpr.state->fin));
	printf("SEQ\t%" PRIu32 "\n", ntohl(s->tcpr.state->seq));
	printf("WIN\t%" PRIu16 "\n", ntohs(s->tcpr.state->win));
	if (s->tcpr.state->peer.have_ack)
		printf("peer ACK\t%" PRIu32 "\n",
		       ntohl(s->tcpr.state->peer.ack));
	if (s->tcpr.state->peer.have_fin)
		printf("peer FIN\t%" PRIu32 "\n",
		       ntohl(s->tcpr.state->peer.fin));
	printf("peer WIN\t%" PRIu16 "\n", ntohs(s->tcpr.state->peer.win));
	if (s->tcpr.state->saved.done_reading)
		printf("done reading\n");
	if (s->tcpr.state->saved.done_writing)
		printf("done writing\n");
	if (s->tcpr.state->done)
		printf("closed\n");
}

static void teardown_state(struct state *s)
{
	int fd;

	if (s->save_file) {
		fd = creat(s->save_file, 0600);
		if (fd < 0) {
			perror("Opening save file");
			exit(EXIT_FAILURE);
		}
		if (write
		    (fd, &s->tcpr.state->saved,
		     sizeof(s->tcpr.state->saved)) !=
		    sizeof(s->tcpr.state->saved)) {
			perror("Saving");
			exit(EXIT_FAILURE);
		}
		if (close(fd) < 0) {
			perror("Closing save file");
			exit(EXIT_FAILURE);
		}
	}

	tcpr_teardown_connection(&s->tcpr);
	if (s->destroy)
		tcpr_destroy_connection(&s->peer_address, s->address.sin_port);
}

int main(int argc, char **argv)
{
	struct state s;

	handle_options(&s, argc, argv);

	setup_state(&s);
	update_state(&s);
	if (s.wait)
		tcpr_wait(&s.tcpr);
	print_state(&s);
	teardown_state(&s);

	return EXIT_SUCCESS;
}
