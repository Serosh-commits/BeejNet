#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#define PORT       "3490"
#define MAXBUFSIZE 1024

static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &(((struct sockaddr_in *)sa)->sin_addr);

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
    int                 sockfd;
    struct addrinfo     hints, *servinfo, *p;
    char                buf[MAXBUFSIZE];
    char                server_ip[INET6_ADDRSTRLEN];
    int                 numbytes;
    int                 rv;
    const char         *host;

    if (argc != 2) {
        fprintf(stderr, "usage: client <hostname>\n");
        exit(1);
    }
    host = argv[1];

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(host, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) {
            perror("client: socket");
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("client: connect");
            close(sockfd);
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "client: failed to connect\n");
        freeaddrinfo(servinfo);
        exit(2);
    }

    if (inet_ntop(p->ai_family,
                  get_in_addr((struct sockaddr *)p->ai_addr),
                  server_ip, sizeof server_ip) == NULL) {
        perror("inet_ntop");
        strcpy(server_ip, "<unknown>");
    }

    printf("=== BeejNet Client ===\n");
    printf("Connected to %s:%s\n", server_ip, PORT);
    printf("Type a message and press Enter (Ctrl+D to quit)\n\n");

    freeaddrinfo(servinfo);

    while (1) {
        printf("you> ");
        fflush(stdout);

        if (fgets(buf, sizeof buf, stdin) == NULL) {
            printf("\n[goodbye]\n");
            break;
        }

        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
            len--;
        }

        if (len == 0)
            continue;

        if (send(sockfd, buf, len, 0) == -1) {
            perror("send");
            break;
        }

        numbytes = recv(sockfd, buf, MAXBUFSIZE - 1, 0);
        if (numbytes <= 0) {
            if (numbytes == 0)
                printf("Server closed connection\n");
            else
                perror("recv");
            break;
        }

        buf[numbytes] = '\0';
        printf("srv> %s\n", buf);
    }

    close(sockfd);
    return 0;
}
