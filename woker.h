#ifndef __WORKER_H
#define __WORKER_H
#include <sys/epoll.h>

int send_to_worker(int sfd, int sockfd);
int recv_from_master(int rfd);
void worker(int sv);

#endif
