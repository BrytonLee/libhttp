#ifndef __PROCESS_H
#define __PROCESS_H
#include "libhttp/mempool.h"
#include "libhttp/http.h"

/*
 * request_read: 处理http连接请求。
 * 参数: 
 *		client: client实例的指针,
 *		sockfd: socket 文件描述符。
 * 返回: 
 *		-1: 表示出错，
 *		1(HTTP_SOCKFD_IN): 表示I/O层监听sockfd读事件。
 *		2(HTTP_SOCKFD_OUT): 表示I/O层监听sockfd写事件。
 *		4(HTTP_SOCKFD_DEL): 表示I/O层删除sockfd事件监听。
 */
int request_read(struct http_client *client, int sockfd);

/*
 * response_write: 回写http响应。 
 * 参数: 
 *		client: client实例的指针,
 *		sockfd: socket 文件描述符。
 * 返回: 
 *		-1: 表示出错，
 *		1(HTTP_SOCKFD_IN): 表示I/O层监听sockfd读事件。
 *		2(HTTP_SOCKFD_OUT): 表示I/O层监听sockfd写事件。
 *		4(HTTP_SOCKFD_DEL): 表示I/O层删除sockfd事件监听。
 */
int response_write(struct http_client *client, int sockfd);
#endif
