#ifndef __WORKER_H
#define __WORKER_H
#include <sys/epoll.h>

#ifdef __HTTP_ACCEPT_LOCK__
#include <semaphore.h>

extern sem_t	*accept_lock;
extern int listenfd;
__inline__ void remove_and_close(int epfd, int sockfd, struct epoll_event *ev);
#endif

int send_to_worker(int sfd, int sockfd);
int recv_from_master(int rfd);
int create_worker(int worker_n, int worker_sv[][2], int closefd);

#define  HTTP_SOCKFD_IN		0x01
#define  HTTP_SOCKFD_OUT	0x02
#define  HTTP_SOCKFD_DEL	0x04

#endif
