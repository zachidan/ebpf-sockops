#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <netdb.h>
#include <linux/aio_abi.h>
#include <errno.h>

#define PROXY_PORT   10001
#define SERVER_PORT  10000

#define TRUE             1
#define FALSE            0

#define SPLICE           0
#define SPLICE_SIZE      512 * 1024

#define AIO              1

inline int io_setup(unsigned nr, aio_context_t *ctxp)
{
   return syscall(__NR_io_setup, nr, ctxp);
}
 
inline int io_destroy(aio_context_t ctx) 
{
   return syscall(__NR_io_destroy, ctx);
}

inline int io_submit(aio_context_t ctx, long nr,  struct iocb **iocbpp) 
{
   return syscall(__NR_io_submit, ctx, nr, iocbpp);
}

#define AIO_RING_MAGIC 0xa10a10a1

struct aio_ring {
    unsigned id; /** kernel internal index number */
    unsigned nr; /** number of io_events */
    unsigned head;
    unsigned tail;

    unsigned magic;
    unsigned compat_features;
    unsigned incompat_features;
    unsigned header_length; /** size of aio_ring */

    struct io_event events[0];
};

#ifdef __x86_64__
#define read_barrier() __asm__ __volatile__("lfence" ::: "memory")
#else
#ifdef __i386__
#define read_barrier() __asm__ __volatile__("" : : : "memory")
#else
#define read_barrier() __sync_synchronize()
#endif
#endif

inline static int io_getevents(aio_context_t ctx, long min_nr, long max_nr,
			       struct io_event *events,
			       struct timespec *timeout)
{
	int i = 0;

	struct aio_ring *ring = (struct aio_ring *)ctx;
	if (ring == NULL || ring->magic != AIO_RING_MAGIC) {
		goto do_syscall;
	}

	while (i < max_nr) {
		unsigned head = ring->head;
		if (head == ring->tail) {
			/* There are no more completions */
			break;
		} else {
			/* There is another completion to reap */
			events[i] = ring->events[head];
			read_barrier();
			ring->head = (head + 1) % ring->nr;
			i++;
		}
	}

	if (i == 0 && timeout != NULL && timeout->tv_sec == 0 &&
	    timeout->tv_nsec == 0) {
		/* Requested non blocking operation. */
		return 0;
	}

	if (i && i >= min_nr) {
		return i;
	}

do_syscall:
	return syscall(__NR_io_getevents, ctx, min_nr - i, max_nr - i,
		       &events[i], timeout);
}

int main (int argc, char *argv[])
{
   int    i, len, rc, on = 1, iter;
   int    listen_sd, max_sd, new_sd, remote_sd;
   int    desc_ready, end_server = FALSE;
   int    close_conn;
#if SPLICE
   int    pipes[2];
#endif
#if AIO
   aio_context_t io_ctx;
   struct iocb io_cb[2];
   struct iocb *io_cbs[2];
   struct io_event io_events[2];
#endif
   char   buffer[65536];
   struct sockaddr_in6 addr;
   struct sockaddr_in proxy_addr;
   struct hostent *proxy_he;
   struct timeval      timeout, start_t, end_t;
   fd_set              master_set, working_set;
   int64_t numbytes;

   if (argc < 2) {
      perror("remote server hostname must be provided");
      exit(-1);
   }

   /*************************************************************/
   /* Open a connection to the remote server                    */
   /*************************************************************/
   if ((proxy_he=gethostbyname(argv[1])) == NULL) {  /* get the host info */
       perror("gethostbyname");
       exit(-1);
   }

   if ((remote_sd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      exit(-1);
   }

   proxy_addr.sin_family = AF_INET;           /* host byte order */
   proxy_addr.sin_port = htons(SERVER_PORT);  /* short, network byte order */
   proxy_addr.sin_addr = *((struct in_addr *)proxy_he->h_addr);
   bzero(&(proxy_addr.sin_zero), 8);         /* zero the rest of the struct */

   if (connect(remote_sd, (struct sockaddr *)&proxy_addr, sizeof(struct sockaddr)) == -1) {
      perror("connect");
      exit(-1);
   }

#if SPLICE
   /*************************************************************/
   /* Open pipe for in-kernel copy                              */
   /*************************************************************/
   if (pipe(pipes) < 0)
   {
      perror("pipe:");
      exit(-1);
   }
#endif

#if AIO
   io_ctx = 0;
   rc = io_setup(128, &io_ctx);
   if (rc < 0) {
      perror("io_setup error");
      exit(-1);
   }

   /* setup I/O control block */
   memset(io_cb, 0, sizeof(io_cb));
   io_cbs[0] = &io_cb[0];
   io_cbs[1] = &io_cb[1];
#endif

   /*************************************************************/
   /* Create an AF_INET6 stream socket to receive incoming      */
   /* connections on                                            */
   /*************************************************************/
   listen_sd = socket(AF_INET6, SOCK_STREAM, 0);
   if (listen_sd < 0)
   {
      perror("socket() failed");
      exit(-1);
   }

   /*************************************************************/
   /* Allow socket descriptor to be reuseable                   */
   /*************************************************************/
   rc = setsockopt(listen_sd, SOL_SOCKET,  SO_REUSEADDR,
                   (char *)&on, sizeof(on));
   if (rc < 0)
   {
      perror("setsockopt() failed");
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
      perror("ioctl() failed");
      close(listen_sd);
      exit(-1);
   }

   /*************************************************************/
   /* Bind the socket                                           */
   /*************************************************************/
   memset(&addr, 0, sizeof(addr));
   addr.sin6_family      = AF_INET6;
   memcpy(&addr.sin6_addr, &in6addr_any, sizeof(in6addr_any));
   addr.sin6_port        = htons(PROXY_PORT);
   rc = bind(listen_sd,
             (struct sockaddr *)&addr, sizeof(addr));
   if (rc < 0)
   {
      perror("bind() failed");
      close(listen_sd);
      exit(-1);
   }

   /*************************************************************/
   /* Set the listen back log                                   */
   /*************************************************************/
   rc = listen(listen_sd, 32);
   if (rc < 0)
   {
      perror("listen() failed");
      close(listen_sd);
      exit(-1);
   }

   /*************************************************************/
   /* Initialize the master fd_set                              */
   /*************************************************************/
   FD_ZERO(&master_set);
   max_sd = listen_sd;
   FD_SET(listen_sd, &master_set);

   /*************************************************************/
   /* Initialize the timeval struct to 3 minutes.  If no        */
   /* activity after 3 minutes this program will end.           */
   /*************************************************************/
   timeout.tv_sec  = 3 * 60;
   timeout.tv_usec = 0;

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
      /* Call select() and wait 3 minutes for it to complete.   */
      /**********************************************************/
      printf("Waiting on select()...\n");
      rc = select(max_sd + 1, &working_set, NULL, NULL, &timeout);

      /**********************************************************/
      /* Check to see if the select call failed.                */
      /**********************************************************/
      if (rc < 0)
      {
         perror("  select() failed");
         break;
      }

      /**********************************************************/
      /* Check to see if the 3 minute time out expired.         */
      /**********************************************************/
      if (rc == 0)
      {
         printf("  select() timed out.  End program.\n");
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
                        perror("  accept() failed");
                        end_server = TRUE;
                     }
                     break;
                  }

                  /**********************************************/
                  /* Add the new incoming connection to the     */
                  /* master read set                            */
                  /**********************************************/
                  printf("  New incoming connection - %d\n", new_sd);
                  gettimeofday(&start_t, NULL);
                  FD_SET(new_sd, &master_set);
                  if (new_sd > max_sd)
                     max_sd = new_sd;

                  /**********************************************/
                  /* Loop back up and accept another incoming   */
                  /* connection                                 */
                  /**********************************************/
               } while (new_sd != -1);
            }

            /****************************************************/
            /* This is not the listening socket, therefore an   */
            /* existing connection must be readable             */
            /****************************************************/
            else
            {
               printf("  Descriptor %d is readable\n", i);
               close_conn = FALSE;
               /*************************************************/
               /* Receive all incoming data on this socket      */
               /* before we loop back and call select again.    */
               /*************************************************/
               do
               {
#if SPLICE
                  /**********************************************/
                  /* Forward the data to the remote server      */
                  /**********************************************/
                  rc = splice(i, NULL, pipes[1], NULL, SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK );
                  if (rc < 0)
                  {
                     if (errno != EAGAIN) {
                        perror("  splice1 failed:");
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
                     printf("  Connection closed\n");
                     close_conn = TRUE;
                     break;
                  }

                  /**********************************************/
                  /* Data was received                          */
                  /**********************************************/
                  len = rc;
                  numbytes+=len;
                  if (iter++ == 100) {
                     gettimeofday(&end_t, NULL);

                     int64_t tdiff_usec = ((end_t.tv_sec * 1000000 + end_t.tv_usec) - (start_t.tv_sec * 1000000 + start_t.tv_usec));
                     float rate = (float)numbytes * 8 / tdiff_usec;
                     printf("%d Mbit/sec\n", (int)rate);
                     iter=0;
                  }
                  //printf("  %d bytes received\n", len);

                  rc = splice(pipes[0], NULL, remote_sd, NULL, SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE);
                  if (rc < 0)
                  {
                     perror("  splice2 failed:");
                     close_conn = TRUE;
                     break;
                  }
#else

#if AIO
                  io_cb[0].aio_fildes = remote_sd;
                  io_cb[0].aio_lio_opcode = IOCB_CMD_PWRITE;
                  io_cb[0].aio_buf = (uint64_t)&buffer[0];
                  io_cb[0].aio_nbytes = 0;

                  io_cb[1].aio_fildes = i;
                  io_cb[1].aio_lio_opcode = IOCB_CMD_PREAD;
                  io_cb[1].aio_buf = (uint64_t)&buffer[0];
                  io_cb[1].aio_nbytes = sizeof(buffer);

                  do {
                     rc = io_submit(io_ctx, 2, io_cbs);

                     rc = io_getevents(io_ctx, 2, 2, io_events, NULL);
                     if (rc != 2) {
                        close_conn = TRUE;
                        break;
                     }
                     if (io_events[1].res == 0) {
                        close_conn = TRUE;
                        break;
                     }
                     len = io_events[1].res;
                     io_cb[0].aio_nbytes = len;
                     numbytes+=len;
                     if (iter++ == 10000) {
                        gettimeofday(&end_t, NULL);

                        int64_t tdiff_usec = ((end_t.tv_sec * 1000000 + end_t.tv_usec) - (start_t.tv_sec * 1000000 + start_t.tv_usec));
                        float rate = (float)numbytes * 8 / tdiff_usec;
                        printf("%d Mbit/sec\n", (int)rate);
                        iter=0;
                     }
                  } while(TRUE);
                  break;
#else
                  /**********************************************/
                  /* Receive data on this connection until the  */
                  /* recv fails with EWOULDBLOCK.  If any other */
                  /* failure occurs, we will close the          */
                  /* connection.                                */
                  /**********************************************/
                  rc = recv(i, buffer, sizeof(buffer), 0);
                  if (rc < 0)
                  {
                     if (errno != EWOULDBLOCK)
                     {
                        perror("  recv() failed");
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
                     printf("  Connection closed\n");
                     close_conn = TRUE;
                     break;
                  }

                  /**********************************************/
                  /* Data was received                          */
                  /**********************************************/
                  len = rc;
                  numbytes+=len;
                  if (iter++ == 100000) {
                     gettimeofday(&end_t, NULL);
                  
                     int64_t tdiff_usec = ((end_t.tv_sec * 1000000 + end_t.tv_usec) - (start_t.tv_sec * 1000000 + start_t.tv_usec));
                     float rate = (float)numbytes * 8 / tdiff_usec;
                     printf("%d Mbit/sec\n", (int)rate);
                     iter=0;
                  }
                  //printf("  %d bytes received\n", len);

                  /**********************************************/
                  /* Forward the data to the remote server      */
                  /**********************************************/
                  rc = send(remote_sd, buffer, len, 0);
                  if (rc < 0)
                  {
                     perror("  send() failed");
                     close_conn = TRUE;
                     break;
                  }
#endif
#endif

               } while (TRUE);

               /*************************************************/
               /* If the close_conn flag was turned on, we need */
               /* to clean up this active connection.  This     */
               /* clean up process includes removing the        */
               /* descriptor from the master set and            */
               /* determining the new maximum descriptor value  */
               /* based on the bits that are still turned on in */
               /* the master set.                               */
               /*************************************************/
               if (close_conn)
               {
                  close(i);
                  FD_CLR(i, &master_set);
                  if (i == max_sd)
                  {
                     while (FD_ISSET(max_sd, &master_set) == FALSE)
                        max_sd -= 1;
                  }
               }
            } /* End of existing connection is readable */
         } /* End of if (FD_ISSET(i, &working_set)) */
      } /* End of loop through selectable descriptors */

   } while (end_server == FALSE);

   /*************************************************************/
   /* Clean up all of the sockets that are open                 */
   /*************************************************************/
   for (i=0; i <= max_sd; ++i)
   {
      if (FD_ISSET(i, &master_set))
         close(i);
   }

   close(remote_sd);
#if SPLICE
   close(pipes[0]);
   close(pipes[1]);
#endif
   return 0;
}
