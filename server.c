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
#include <sys/wait.h>

#define PORT       "3490"
#define BACKLOG    10
#define MAXBUFSIZE 1024

static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &(((struct sockaddr_in *)sa)->sin_addr);

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

static unsigned short get_in_port(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return ntohs(((struct sockaddr_in *)sa)->sin_port);

    return ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
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

int main(void)
{
    int                 sockfd, clientfd;
    struct addrinfo     hints, *servinfo, *p;
    struct sockaddr_storage client_addr;
    socklen_t           addr_len;
    char                buf[MAXBUFSIZE];
    char                client_ip[INET6_ADDRSTRLEN];
    int                 numbytes;
    int                 yes = 1;
    int                 rv;

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
    hints.ai_flags    = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            close(sockfd);
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("server: bind");
            close(sockfd);
            continue;
        }

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

    printf("=== BeejNet Server ===\n");
    printf("Listening on port %s  (Ctrl+C to quit)\n\n", PORT);

    while (1) {
        addr_len = sizeof client_addr;
        clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);
        if (clientfd == -1) {
            perror("accept");
            continue;
        }

        if (inet_ntop(client_addr.ss_family,
                      get_in_addr((struct sockaddr *)&client_addr),
                      client_ip, sizeof client_ip) == NULL) {
            perror("inet_ntop");
            strcpy(client_ip, "<unknown>");
        }

        printf("[+] Connection from %s:%u\n",
               client_ip,
               get_in_port((struct sockaddr *)&client_addr));

        while (1) {
            numbytes = recv(clientfd, buf, MAXBUFSIZE - 1, 0);
            if (numbytes <= 0) {
                if (numbytes == 0)
                    printf("[-] Client %s hung up\n", client_ip);
                else
                    perror("recv");
                break;
            }

            buf[numbytes] = '\0';
            printf("    ← recv %d bytes: \"", numbytes);
            print_escaped_payload(buf, (size_t)numbytes);
            printf("\"\n");

            if (send(clientfd, buf, numbytes, 0) == -1) {
                perror("send");
                break;
            }
            printf("    → echoed back\n");
        }

        close(clientfd);
        printf("    [closed]\n\n");
    }

    close(sockfd);
    return 0;
}
