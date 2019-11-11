#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <errno.h>

#define PROXY_PORT   10001
#define SERVER_PORT  10000
#define MAX_THREADS      8
#define NUM_THREADS      4
#define NUM_CONNS       32
#define MAX_FDS        256

#define TRUE             1
#define FALSE            0

#define SPLICE           0
#define SPLICE_SIZE 262144

#define EPOLL            0
#define EPOLL_MAXEVENTS 64

#define max(x,y) (x > y ? x : y)

int sig_pipes[2];

struct proxy_args {
   int tid;
   int ctrl_fd;
};

void *proxy_th(void *pargs_void_ptr)
{
    int    tid, ctrl_fd, ready_fd, end_thread, desc_ready, rc;
    int    fd_map[MAX_FDS];
#if SPLICE
    int    pipes[2];
#else
    char   buffer[262 * 1024];
#endif

#if EPOLL
    int epoll_fd;
    struct epoll_event event, *events;
#else
    int max_fd;
    fd_set master_set, working_set;
#endif

    struct proxy_args *pargs_ptr = (struct proxy_args *)pargs_void_ptr;
    end_thread = FALSE;
    tid = pargs_ptr->tid;
    ctrl_fd = pargs_ptr->ctrl_fd;
    memset(fd_map, 0, sizeof(fd_map));
#if EPOLL
    events = malloc(EPOLL_MAXEVENTS * sizeof(struct epoll_event));
    if (events == NULL) {
       perror("   malloc:");
       return NULL;
    }
    memset(events, 0, EPOLL_MAXEVENTS * sizeof(event));

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
       perror("   epoll_create:");
       return NULL;
   }
   event.events = EPOLLIN;
   event.data.fd = ctrl_fd;

   if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &event))
   {
      perror("   epoll_ctl:");
      close(epoll_fd);
      return NULL;
   }
#else
    FD_ZERO(&master_set);
    max_fd = ctrl_fd;
    FD_SET(ctrl_fd, &master_set);
#endif

    printf("Thread %d has been started\n", tid);

#if SPLICE
    printf("Splice is enabled - %d bytes\n", SPLICE_SIZE);
    if (pipe(pipes) < 0) {
       perror("   pipe:");
       return NULL;
    }
#endif

    do
    {
#if EPOLL
      desc_ready = epoll_wait(epoll_fd, events, EPOLL_MAXEVENTS, 100);
#else
      memcpy(&working_set, &master_set, sizeof(master_set));
      rc = select(max_fd + 1, &working_set, NULL, NULL, NULL);

      /**********************************************************/
      /* Check to see if the select call failed.                */
      /**********************************************************/
      if (rc < 0)
      {
         if (errno != EINTR)
            perror("  select:");
         end_thread = TRUE;
         break;
      }

      /**********************************************************/
      /* Check to see if the time out expired.                  */
      /**********************************************************/
      if (rc == 0)
      {
         printf("  select() timed out.  End thread.\n");
         end_thread = TRUE;
         break;
      }
#endif

      /**********************************************************/
      /* One or more descriptors are readable.  Need to         */
      /* determine which ones they are.                         */
      /**********************************************************/
#if EPOLL
      for (int i=0; i < desc_ready; i++)
      {
         if (events[i].events & EPOLLHUP || events[i].events & EPOLLERR) {
            end_thread = TRUE;
            break;
         }

         if (events[i].events & EPOLLIN) {
            ready_fd = events[i].data.fd;
#else
      desc_ready = rc;
      for (int i=0; i <= max_fd && desc_ready > 0; ++i)
      {
         if (FD_ISSET(i, &working_set)) {
            desc_ready--;
            ready_fd = i;
#endif
            if (ready_fd == ctrl_fd) {
               int fd1, fd2;
               rc = read(ctrl_fd, &fd1, sizeof(fd1));
               if (rc <= 0) {
                  if (rc < 0)
                     perror("   read:");
                  end_thread = TRUE;
                  break;
               }

               rc = read(ctrl_fd, &fd2, sizeof(int));
               if (rc < 0) {
                  perror("   read:");
                  end_thread = TRUE;
                  break;
               }

               if (fd1 >= MAX_FDS || fd2 >= MAX_FDS) {
                  printf("   no more space in fd_map\n");
                  end_thread = TRUE;
                  break;
               }

               fd_map[fd1] = fd2;
               fd_map[fd2] = fd1;
#if EPOLL
               event.data.fd = fd1;
               event.events = EPOLLIN;
               if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd1, &event) == -1) {
                  perror("   epoll_ctl:");
                  end_thread = TRUE;
                  break;
               }
               event.data.fd = fd2;
               event.events = EPOLLIN | EPOLLET;
               if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd2, &event) == -1) {
                  perror("   epoll_ctl:");
                  end_thread = TRUE;
                  break;
               }
#else
               FD_SET(fd1, &master_set);
               FD_SET(fd2, &master_set);
               if (max_fd < fd1) {
                  max_fd = fd1;
               }
               if (max_fd < fd2) {
                  max_fd = fd2;
               }
#endif
            }
            else
            {
               int close_conn = FALSE;
               do {
#if SPLICE
                  /**********************************************/
                  /* Forward the data to the remote server      */
                  /**********************************************/
                  rc = splice(ready_fd, NULL, pipes[1], NULL, SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
                  if (rc < 0)
                  {
                     if (errno != EAGAIN) {
                        perror("  splice1:");
                        close_conn = TRUE;
                     }
                     break;
                  }

                  /**********************************************/
                  /* Check to see if the connection has been    */
                  /* closed by the client                       */
                  /**********************************************/
                  if (rc == 0)
                  {
                     printf("  Connection %d closed\n", ready_fd);
                     close_conn = TRUE;
                     break;
                  }

                  rc = splice(pipes[0], NULL, fd_map[ready_fd], NULL, SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
                  if (rc < 0)
                  {
                     perror("  splice2:");
                     close_conn = TRUE;
                     break;
                  }
#else
                  rc = recv(ready_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                  if (rc < 0)
                  {
                     if (errno != EWOULDBLOCK)
                     {
                        perror("  recv:");
                        close_conn = TRUE;
                     }
                     break;
                  }

                  /**********************************************/
                  /* Check to see if the connection has been    */
                  /* closed by the client                       */
                  /**********************************************/
                  if (rc == 0)
                  {
                     printf("  Connection %d closed\n", ready_fd);
                     close_conn = TRUE;
                     break;
                  }

                  int nbytes = rc;

                  /**********************************************/
                  /* Forward the data to the remote server      */
                  /**********************************************/
                  rc = send(fd_map[ready_fd], buffer, nbytes, MSG_DONTWAIT);
                  if (rc < 0)
                  {
                     perror("  send:");
                     close_conn = TRUE;
                     break;
                  }
                  if (rc != nbytes)
                     printf("Incomplete send %d < %d\n", rc, nbytes);
#endif
               } while (close_conn == FALSE);

               if (close_conn == TRUE) {
#if EPOLL
                  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ready_fd, NULL) == -1) {
                     perror("   epoll_ctl:");
                  }
                  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd_map[ready_fd], NULL) == -1) {
                     perror("   epoll_ctl:");
                  }
#else
                  FD_CLR(ready_fd, &master_set);
                  FD_CLR(fd_map[ready_fd], &master_set); 
#endif
                  close(ready_fd);
                  close(fd_map[ready_fd]);
                  fd_map[fd_map[ready_fd]] = 0;
                  fd_map[ready_fd] = 0;
               }
            }
         }
      }
   } while (end_thread == FALSE);

   /*************************************************************/
   /* Clean up all of the sockets that are open                 */
   /*************************************************************/
#if !EPOLL
   for (int i=0; i <= max_fd; ++i)
   {
      if (FD_ISSET(i, &master_set))
         close(i);
   }
#endif

   printf("Thread %d stopped\n", tid);
   return NULL;
}

// Handler for SIGINT, caused by  Ctrl+C at keyboard
void handle_sigint(int sig) 
{
   printf("Caught signal %d\n", sig); 
   if (sig_pipes[1] != 0) {
     close(sig_pipes[1]);
   }
}

int open_conn(struct hostent *dest_he, int port) {
   int sock, on=1;
   struct sockaddr_in  proxy_addr;

   if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket:");
      return -1;
   }

   proxy_addr.sin_family = AF_INET;           /* host byte order */
   proxy_addr.sin_port = htons(port)    ;     /* short, network byte order */
   proxy_addr.sin_addr = *((struct in_addr *)dest_he->h_addr);
   memset(&(proxy_addr.sin_zero), 0, 8);      /* zero the rest of the struct */

   if (connect(sock, (struct sockaddr *)&proxy_addr, sizeof(struct sockaddr)) == -1) {
      perror("connect:");
      close(sock);
      return -1;
   }

   int rc = ioctl(sock, FIONBIO, (char *)&on);
   if (rc < 0)
   {
      perror("ioctl:");
      close(sock);
      return -1;
   }

   return sock;
}

int main (int argc, char *argv[])
{
   int    i, rc, on = 1;
   int    listen_sd, max_sd, new_sd, curr_thread=0;
   int    desc_ready, end_server = FALSE;
   int    port_in = PROXY_PORT, port_out = SERVER_PORT, nthreads = NUM_THREADS, nconn=0;
   int    tid_fd[MAX_THREADS], conns[NUM_CONNS];
   char  *hostname;
   struct sockaddr_in6 addr;
   struct sockaddr_in  proxy_addr;
   struct hostent     *proxy_he;
   fd_set              master_set, working_set;
   pthread_t           tid[MAX_THREADS];
   struct proxy_args   pargs[MAX_THREADS];

   if (argc < 2) {
      printf("remote server hostname must be provided\n");
      exit(-1);
   }
   hostname = argv[1];

   if (argc > 2) {
      port_in = atoi(argv[2]);
   }

   if (argc > 3) {
      port_out = atoi(argv[3]);
   }

   if (argc > 4) {
      nthreads = atoi(argv[4]);
      if (nthreads > MAX_THREADS) {
         nthreads = MAX_THREADS;
      }
   }

   if (pipe(sig_pipes) < 0) {
      perror("pipe:");
      exit(-1);
   }

   signal(SIGINT, handle_sigint); 

   /*************************************************************/
   /* Open NUM_CONNS connections to the remote server           */
   /*************************************************************/
   if ((proxy_he=gethostbyname(argv[1])) == NULL) {  /* get the host info */
       perror("gethostbyname:");
       exit(-1);
   }

   memset(conns, 0, sizeof(conns));
   for (i=0; i < NUM_CONNS; i++) {
      conns[i] = open_conn(proxy_he, port_out);
      if (conns[i] == -1) {
         exit(-1);
      }
   }

   /*************************************************************/
   /* Create worker threads                                     */
   /*************************************************************/
   memset(&tid, 0, sizeof(tid));
   memset(&tid_fd, 0, sizeof(tid_fd));
   memset(&pargs, 0, sizeof(pargs));

   int pipes[2];
   for (i=0; i < nthreads; i++) {
      if (pipe(pipes) < 0) {
         perror("   pipe:");
         exit(-1);
      }

      tid_fd[i] = pipes[1];
      pargs[i].tid = i;
      pargs[i].ctrl_fd = pipes[0];
      rc = pthread_create(&tid[i], NULL, proxy_th, (void *)&pargs[i]);
      if (rc < 0) {
         perror("   pthread_create:");
         exit(-1);
      }
   }

   /*************************************************************/
   /* Create an AF_INET6 stream socket to receive incoming      */
   /* connections on                                            */
   /*************************************************************/
   listen_sd = socket(AF_INET6, SOCK_STREAM, 0);
   if (listen_sd < 0)
   {
      perror("socket:");
      exit(-1);
   }

   /*************************************************************/
   /* Allow socket descriptor to be reuseable                   */
   /*************************************************************/
   rc = setsockopt(listen_sd, SOL_SOCKET,  SO_REUSEADDR,
                   (char *)&on, sizeof(on));
   if (rc < 0)
   {
      perror("setsockopt:");
      close(listen_sd);
      exit(-1);
   }

   /*************************************************************/
   /* Set socket to be nonblocking. All of the sockets for      */
   /* the incoming connections will also be nonblocking since   */
   /* they will inherit that state from the listening socket.   */
   /*************************************************************/
   rc = ioctl(listen_sd, FIONBIO, (char *)&on);
   if (rc < 0)
   {
      perror("ioctl:");
      close(listen_sd);
      exit(-1);
   }

   /*************************************************************/
   /* Bind the socket                                           */
   /*************************************************************/
   memset(&addr, 0, sizeof(addr));
   addr.sin6_family      = AF_INET6;
   memcpy(&addr.sin6_addr, &in6addr_any, sizeof(in6addr_any));
   addr.sin6_port        = htons(port_in);
   rc = bind(listen_sd,
             (struct sockaddr *)&addr, sizeof(addr));
   if (rc < 0)
   {
      perror("bind:");
      close(listen_sd);
      exit(-1);
   }

   /*************************************************************/
   /* Set the listen back log                                   */
   /*************************************************************/
   rc = listen(listen_sd, 32);
   if (rc < 0)
   {
      perror("listen:");
      close(listen_sd);
      exit(-1);
   }

   /*************************************************************/
   /* Initialize the master fd_set                              */
   /*************************************************************/
   FD_ZERO(&master_set);
   max_sd = max(listen_sd, sig_pipes[0]);
   FD_SET(listen_sd, &master_set);
   FD_SET(sig_pipes[0], &master_set);

   /*************************************************************/
   /* Loop waiting for incoming connects or for incoming data   */
   /* on any of the connected sockets.                          */
   /*************************************************************/
   do
   {
      /**********************************************************/
      /* Copy the master fd_set over to the working fd_set.     */
      /**********************************************************/
      memcpy(&working_set, &master_set, sizeof(master_set));

      /**********************************************************/
      /* Call select() and wait 5 minutes for it to complete.   */
      /**********************************************************/
      printf("Waiting on select()...\n");
      rc = select(max_sd + 1, &working_set, NULL, NULL, NULL);

      /**********************************************************/
      /* Check to see if the select call failed.                */
      /**********************************************************/
      if (rc < 0)
      {
         if (errno != EINTR) {
            perror("  select:");
         }
         end_server = TRUE;
         break;
      }

      /**********************************************************/
      /* One or more descriptors are readable.  Need to         */
      /* determine which ones they are.                         */
      /**********************************************************/
      desc_ready = rc;
      for (i=0; i <= max_sd  &&  desc_ready > 0; ++i)
      {
         /*******************************************************/
         /* Check to see if this descriptor is ready            */
         /*******************************************************/
         if (FD_ISSET(i, &working_set))
         {
            /****************************************************/
            /* A descriptor was found that was readable - one   */
            /* less has to be looked for.  This is being done   */
            /* so that we can stop looking at the working set   */
            /* once we have found all of the descriptors that   */
            /* were ready.                                      */
            /****************************************************/
            desc_ready -= 1;

            /****************************************************/
            /* Check to see if this is the listening socket     */
            /****************************************************/
            if (i == listen_sd)
            {
               printf("  Listening socket is readable\n");
               /*************************************************/
               /* Accept all incoming connections that are      */
               /* queued up on the listening socket before we   */
               /* loop back and call select again.              */
               /*************************************************/
               do
               {
                  /**********************************************/
                  /* Accept each incoming connection.  If       */
                  /* accept fails with EWOULDBLOCK, then we     */
                  /* have accepted all of them.  Any other      */
                  /* failure on accept will cause us to end the */
                  /* server.                                    */
                  /**********************************************/
                  new_sd = accept(listen_sd, NULL, NULL);
                  if (new_sd < 0)
                  {
                     if (errno != EWOULDBLOCK)
                     {
                        perror("  accept:");
                        end_server = TRUE;
                     }
                     break;
                  }

                  printf("  New incoming connection - %d\n", new_sd);
                  if (nconn >= NUM_CONNS) {
                     printf("No more free upstream connections\n");
                     close(new_sd);
                     continue;
                  }

                  rc = ioctl(new_sd, FIONBIO, (char *)&on);
                  if (rc < 0)
                  {
                     perror("   ioctl:");
                     close(new_sd);
                     continue;
                  }
                  rc = write(tid_fd[curr_thread], &new_sd, sizeof(int));
                  if (rc < 0) {
                     perror("   write:");
                     close(new_sd);
                     continue;
                  }
                  rc = write(tid_fd[curr_thread], &conns[nconn], sizeof(int));
                  if (rc < 0) {
                     perror("   write:");
                     close(new_sd);
                     continue;
                  }

                  curr_thread = (curr_thread + 1) % nthreads;
                  nconn++;

                  /**********************************************/
                  /* Loop back up and accept another incoming   */
                  /* connection                                 */
                  /**********************************************/
               } while (new_sd != -1);
            }
            /****************************************************/
            /* Check to see if this is the sig_pipes            */
            /****************************************************/
            else if (i == sig_pipes[0])
            {
               end_server = TRUE;
               break; 
            }
         } /* End of if (FD_ISSET(i, &working_set)) */
      } /* End of loop through selectable descriptors */

   } while (end_server == FALSE);

   for (i=0; i < nthreads; i++) {
      close(tid_fd[i]);
   }
   close(listen_sd);

   for (i=0; i < nthreads; i++) {
      pthread_join(tid[i], NULL);
   }

   return 0;
}
