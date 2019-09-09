#include <stdio.h> 
#include <stdlib.h> 
#include <errno.h> 
#include <string.h> 
#include <netdb.h> 
#include <time.h>
#include <sys/time.h>
#include <sys/types.h> 
#include <netinet/in.h> 
#include <sys/socket.h> 
#include <unistd.h>

#define DEFAULT_PORT 10000    /* the port client will be connecting to */

int main(int argc, char *argv[])
{
    int sockfd, iter, port=DEFAULT_PORT;
    uint64_t numbytes;
    char buf[65536];
    struct hostent *he;
    struct sockaddr_in their_addr; /* connector's address information */
    struct timeval start, end;

    if (argc < 2) {
        fprintf(stderr,"usage: client hostname\n");
        exit(1);
    }

    if (argc > 2) {
        port = atoi(argv[2]);
    }

    if ((he=gethostbyname(argv[1])) == NULL) {  /* get the host info */
        herror("gethostbyname");
        exit(1);
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    their_addr.sin_family = AF_INET;      /* host byte order */
    their_addr.sin_port = htons(port);    /* short, network byte order */
    their_addr.sin_addr = *((struct in_addr *)he->h_addr);
    bzero(&(their_addr.sin_zero), 8);     /* zero the rest of the struct */

    if (connect(sockfd, (struct sockaddr *)&their_addr, \
                                          sizeof(struct sockaddr)) == -1) {
        perror("connect");
        exit(1);
    }
    gettimeofday(&start, NULL);
    while (1) {
        if (send(sockfd, buf, 65536, 0) == -1){
            perror("send");
            sleep(120);
            exit(1);
        }
        numbytes += 65536;
        iter++;
        if (iter == 100000) {
            gettimeofday(&end, NULL);
            int64_t tdiff_usec = ((end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec));
            float rate = (float)numbytes * 8 / tdiff_usec;
            printf("%ld bytes, %ld usec, %d Mbit/sec\n", numbytes, tdiff_usec, (int)rate);
            iter=0;
        }
    }

    close(sockfd);

    return 0;
}
