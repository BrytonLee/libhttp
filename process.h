#ifndef __PROCESS_H
#define __PROCESS_H
#include "libhttp/mempool.h"
#include "libhttp/http.h"

int request_process(struct http_client *client, int epfd, int sockfd, struct epoll_event *ev);
int data_process(struct http_client *client, int epfd, int sockfd, struct epoll_event *ev);
int response_write_back(struct http_client *client, int epfd, int sockfd, struct epoll_event *ev);
#endif
