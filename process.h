#ifndef __PROCESS_H
#define __PROCESS_H

int request_process(struct http_client **client, int epfd, int sockfd);
#endif
