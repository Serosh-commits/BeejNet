/*
 * Compile:  gcc client.c -o client -levent
 * Run:      ./client                        (connects to 127.0.0.1:3490)
 *           ./client 192.168.1.10 3490      (custom host/port)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "3490"
#define MAXBUFSIZE   1024


static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &(((struct sockaddr_in *)sa)->sin_addr);

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

static void sigpipe_handler(int sig)
{
    (void)sig;
}

static void print_escaped_payload(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];

        if (c == '\\') {
            fputs("\\\\", stdout);
        } else if (c == '"') {
            fputs("\\\"", stdout);
        } else if (isprint(c)) {
            putchar(c);
        } else {
            printf("\\x%02X", c);
        }
    }
}
static void readcb(struct bufferevent *bev, void *ctx)
{
    struct evbuffer *input = bufferevent_get_input(bev);
    char             buf[MAXBUFSIZE];
    int              n;

    (void)ctx;

    while (evbuffer_get_length(input) > 0) {
        n = evbuffer_remove(input, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';
        printf("    <- recv %d bytes: \"", n);
        print_escaped_payload(buf, (size_t)n);
        printf("\"\n");
    }
}
static void eventcb(struct bufferevent *bev, short events, void *ctx)
{
    struct event_base *base = ctx;

    if (events & BEV_EVENT_CONNECTED) {
        printf("[+] Connected to server\n");
        printf("    Type a message and press Enter. Ctrl+D to quit.\n\n");
        return;   /* connection is live — nothing to tear down */
    }

    if (events & BEV_EVENT_EOF) {
        printf("\n[-] Server closed the connection\n");
    } else if (events & BEV_EVENT_ERROR) {
        int err = EVUTIL_SOCKET_ERROR();
        fprintf(stderr, "[-] Connection error: %s\n",
                evutil_socket_error_to_string(err));
    } else if (events & BEV_EVENT_TIMEOUT) {
        printf("\n[-] Connection timed out\n");
    }

    bufferevent_free(bev);
    event_base_loopexit(base, NULL);
}
static void stdin_cb(evutil_socket_t fd, short event, void *ctx)
{
    struct bufferevent *bev  = ctx;
    char                buf[MAXBUFSIZE];
    ssize_t             n;

    (void)event;

    n = read(fd, buf, sizeof(buf) - 1);

    if (n < 0) {
        perror("read(stdin)");
        bufferevent_free(bev);
        event_base_loopexit(bufferevent_get_base(bev), NULL);
        return;
    }

    if (n == 0) {
        /* Ctrl+D — EOF on stdin */
        printf("\n[*] EOF on stdin — disconnecting\n");
        bufferevent_free(bev);
        event_base_loopexit(bufferevent_get_base(bev), NULL);
        return;
    }

    buf[n] = '\0';

    /* Log what we're sending (mirrors the server's send log) */
    printf("    -> send %zd bytes: \"", n);
    print_escaped_payload(buf, (size_t)n);
    printf("\"\n");

    /* Push into the output evbuffer — Libevent sends it */
    bufferevent_write(bev, buf, n);
}
static void sigint_cb(evutil_socket_t sig, short flags, void *arg)
{
    struct event_base *base = arg;
    (void)sig; (void)flags;
    printf("\nSIGINT received — disconnecting.\n");
    event_base_loopexit(base, NULL);
}

int main(int argc, char *argv[])
{
    const char      *host = (argc > 1) ? argv[1] : DEFAULT_HOST;
    const char      *port = (argc > 2) ? argv[2] : DEFAULT_PORT;

    struct addrinfo  hints, *servinfo, *p;
    int              rv;
    char             server_ip[INET6_ADDRSTRLEN];
    struct sigaction sa_pipe;
    sa_pipe.sa_handler = sigpipe_handler;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa_pipe, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        exit(1);
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if (inet_ntop(p->ai_family,
                      get_in_addr(p->ai_addr),
                      server_ip, sizeof server_ip) != NULL)
            break;
    }
    if (p == NULL) {
        fprintf(stderr, "client: failed to resolve %s\n", host);
        freeaddrinfo(servinfo);
        return 1;
    }

    struct event_base *base = event_base_new();
    if (!base) {
        fprintf(stderr, "event_base_new failed\n");
        freeaddrinfo(servinfo);
        return 1;
    }
    struct bufferevent *bev = bufferevent_socket_new(base, -1,
                                                     BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        fprintf(stderr, "bufferevent_socket_new failed\n");
        event_base_free(base);
        freeaddrinfo(servinfo);
        return 1;
    }
    bufferevent_setcb(bev, readcb, NULL, eventcb, base);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
    struct timeval tv = {30, 0};
    bufferevent_set_timeouts(bev, &tv, NULL);

    if (bufferevent_socket_connect(bev, p->ai_addr, p->ai_addrlen) < 0) {
        fprintf(stderr, "bufferevent_socket_connect failed\n");
        bufferevent_free(bev);
        event_base_free(base);
        freeaddrinfo(servinfo);
        return 1;
    }

    freeaddrinfo(servinfo);
    struct event *stdin_ev = event_new(base, STDIN_FILENO,
                                       EV_READ | EV_PERSIST,
                                       stdin_cb, bev);
    event_add(stdin_ev, NULL);

    /* Graceful Ctrl+C */
    struct event *sig_ev = evsignal_new(base, SIGINT, sigint_cb, base);
    event_add(sig_ev, NULL);

    printf("=== BeejNet Client ===\n");
    printf("Connecting to %s:%s ...\n", server_ip, port);
    event_base_dispatch(base);

    /* Cleanup */
    event_free(stdin_ev);
    event_free(sig_ev);
    event_base_free(base);

    printf("[*] Disconnected.\n");
    return 0;
}
