/*
 * Compile: gcc server.c -o server -levent
 * Test:    nc 127.0.0.1 3490
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>

#define PORT       "3490"
#define BACKLOG    10
#define MAXBUFSIZE 1024


static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &((struct sockaddr_in *)sa)->sin_addr;
    return &((struct sockaddr_in6 *)sa)->sin6_addr;
}

static unsigned int get_in_port(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return ntohs(((struct sockaddr_in *)sa)->sin_port);
    return ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
}


typedef struct {
    char         client_ip[INET6_ADDRSTRLEN];
    unsigned int client_port;
} conn_ctx_t;

static void readcb(struct bufferevent *bev, void *ctx)
{
    conn_ctx_t      *c      = ctx;
    struct evbuffer *input  = bufferevent_get_input(bev);
    struct evbuffer *output = bufferevent_get_output(bev);
    char             buf[MAXBUFSIZE];
    int              n;

    while ((n = evbuffer_remove(input, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("    <- recv %d bytes from %s:%u: \"%s\"\n",
               n, c->client_ip, c->client_port, buf);

        /* Echo: write to the output evbuffer.
           Libevent drains it to the socket automatically when writable —
           no manual send() needed, no partial-write handling.               */
        evbuffer_add(output, buf, n);
        printf("    -> echoed back\n");
    }
}

/* Event callback */

static void eventcb(struct bufferevent *bev, short events, void *ctx)
{
    conn_ctx_t *c = ctx;

    if (events & BEV_EVENT_EOF)
        printf("[-] Client %s:%u hung up\n", c->client_ip, c->client_port);
    else if (events & BEV_EVENT_ERROR)
        fprintf(stderr, "[-] Client %s:%u error: %s\n",
                c->client_ip, c->client_port,
                evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    else if (events & BEV_EVENT_TIMEOUT)
        printf("[-] Client %s:%u idle timeout\n", c->client_ip, c->client_port);

    printf("    [closed]\n\n");
    free(c);
    bufferevent_free(bev);
}

/*Accept callback */

static void accept_cb(
    struct evconnlistener *listener,
    evutil_socket_t        fd,
    struct sockaddr       *addr,
    int                    addrlen,
    void                  *ctx
) {
    struct event_base *base = evconnlistener_get_base(listener);

    /* Allocate per-connection state — stays in heap memory */
    conn_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) { EVUTIL_CLOSESOCKET(fd); return; }
    inet_ntop(addr->sa_family, get_in_addr(addr),
              c->client_ip, sizeof(c->client_ip));
    c->client_port = get_in_port(addr);

    printf("[+] Connection from %s:%u\n", c->client_ip, c->client_port);

  
    struct bufferevent *bev = bufferevent_socket_new(
        base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        fprintf(stderr, "bufferevent_socket_new failed\n");
        free(c);
        EVUTIL_CLOSESOCKET(fd);
        return;
    }

    /* Register the three callbacks */
    bufferevent_setcb(bev, readcb, NULL, eventcb, c);
  
    bufferevent_setwatermark(bev, EV_READ, 1, MAXBUFSIZE);
    
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

static void accept_err_cb(struct evconnlistener *l, void *ctx)
{
    int err = EVUTIL_SOCKET_ERROR();
    fprintf(stderr, "Accept error %d: %s\n", err,
            evutil_socket_error_to_string(err));
    (void)l; (void)ctx;
}

/* SIGINT: graceful shutdown using CTRL+C   */

static void sigint_cb(evutil_socket_t sig, short flags, void *arg)
{
    struct event_base *base = arg;
    printf("\nSIGINT — shutting down gracefully\n");
    event_base_loopexit(base, NULL);
    (void)sig; (void)flags;
}
int main(void)
{
    int              sockfd, yes = 1, rv;
    struct addrinfo  hints, *servinfo, *p;
    struct sigaction sa_pipe;

    /* SIGPIPE */
    memset(&sa_pipe, 0, sizeof sa_pipe);
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa_pipe, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        exit(1);
    }

   // Clear hints and set preferences: TCP server, auto-fill my IP
memset(&hints, 0, sizeof hints);
hints.ai_family   = AF_UNSPEC;
hints.ai_socktype = SOCK_STREAM;
hints.ai_flags    = AI_PASSIVE;

// Look up available addresses to bind to
if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return 1;
}

// Try each address until one works
for (p = servinfo; p != NULL; p = p->ai_next) {

    // Create a socket
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
        perror("server: socket");
        continue;
    }

    // Allow port reuse so we don't get "Address already in use" on restart
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("setsockopt");
        close(sockfd);
        exit(1);
    }

    // Bind the socket to the port
    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
        close(sockfd);
        perror("server: bind");
        continue;
    }

    // Bound successfully, stop trying
    break;
}

    freeaddrinfo(servinfo);

    if (p == NULL) {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }
    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        close(sockfd);
        exit(1);
    }

    /* Libevent setup */

    struct event_base *base = event_base_new();
    if (!base) {
        fprintf(stderr, "event_base_new failed\n");
        close(sockfd);
        return 1;
    }

    evutil_make_socket_nonblocking(sockfd);

    /* SIGINT handler — clean shutdown instead of Ctrl+C killing mid-loop */
    struct event *sig_ev = evsignal_new(base, SIGINT, sigint_cb, base);
    event_add(sig_ev, NULL);

    /*
     * Hand the already-bound, already-listening sockfd to evconnlistener.
     * evconnlistener_new  (not _new_bind) takes an existing fd.
     * backlog = -1  =>  skip the listen() call (we already called it above).
     * LEV_OPT_CLOSE_ON_FREE  =>  evconnlistener_free() closes sockfd for us.
     * LEV_OPT_REUSEABLE  =>  sets SO_REUSEADDR (already set above, harmless).
     */
    struct evconnlistener *listener = evconnlistener_new(
        base,
        accept_cb,
        NULL,                                       /* ctx for accept_cb */
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
        -1,                                         /* skip re-listen */
        sockfd
    );
    if (!listener) {
        fprintf(stderr, "evconnlistener_new failed\n");
        event_free(sig_ev);
        event_base_free(base);
        close(sockfd);
        return 1;
    }
    evconnlistener_set_error_cb(listener, accept_err_cb);

    printf("=== BeejNet Server ===\n");
    printf("Backend  : %s\n", event_base_get_method(base));
    printf("Listening on port %s  (Ctrl+C to quit)\n\n", PORT);

    /*
     * Replaces while(1).
     * Sleeps in epoll_wait/kqueue, wakes only when a socket is ready.
     * All accept/read/write/close happens through the callbacks above.
     */
    event_base_dispatch(base);
    evconnlistener_free(listener);   /* also closes sockfd */
    event_free(sig_ev);
    event_base_free(base);
    return 0;
}
