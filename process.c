#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include "process.h"

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
int request_read(struct http_client *client, int sockfd)
{
	struct pool_entry *mem;
	int total_read = 0, ret = -1;
	int	mem_inuse = 0; 
	unsigned int	epflag = 0;

	if ( NULL == client || sockfd < -1 )
		return ret;
	
	mem = client->inbuff;

	if ( !client->inprocess ) {
		/* 新请求 */
		client->inprocess = 1;
	} else {
		if ( mem->inuse_size ) {
			mem_inuse = mem->inuse_size;
			ret = http_client_head_valid(client);
			if ( ret == 1 ) {
				/* 判断有没有读取到请求完整的content */
				ret = http_client_req_entire(client);
				if ( !ret ) {
					total_read = http_client_current_buff(client, mem);
				} 
			} else if ( ret == 0) 
				/* 还没有读取到完整的头域 */
				total_read += mem_inuse;
			else
				return ret;
		} else {
			return ret;
		}
	}
	
	while ( 1 ) {
		ret = read(sockfd, mem->buff + total_read, HTTP_MAX_HEAD_LEN - total_read);
		if ( -1 == ret  && errno == EINTR ) 
			continue;
		else if ( -1 == ret && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* 目前读不到数据了 */
			break;
		} else if ( -1 == ret ) {
			perror("read error: ");
			return ret;
		} else if ( 0 == ret ) {
			fprintf(stderr, "remote closed socket\n");
			return ret;
		}
		total_read += ret;
	}

	mem->inuse_size = total_read;
	*((char *)mem->buff + total_read)  = '\0';

	/* 判断是否读取到整个HTTP 头域 */
	ret = http_client_head_valid(client);
	if ( ret < 0 )
		return ret;
	else if ( ret == 0 ) {
		/* 没有读取到完整的头域，等待下一步处理 */
		return HTTP_SOCKFD_IN;
	} else {
		/* 已经读取到完整的头部 */
		if ( (ret = http_client_parse(client)) < 0 )
			return ret;

		/* 处理请求 */
		ret = http_client_request(client, &epflag);
		if ( ret == -1 )
			return ret;

		if ( HTTP_SOCKFD_OUT == epflag ) {
			return HTTP_SOCKFD_OUT;
		} else if ( HTTP_SOCKFD_IN == epflag ) {
			return HTTP_SOCKFD_IN;
		} else if ( HTTP_SOCKFD_DEL == epflag ) {
			return HTTP_SOCKFD_DEL;
		} else {
			/* 出错了 */
			return -1;
		}
	}
}

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
int response_write(struct http_client *client, int sockfd)
{
	struct pool_entry *entry;
	int ret = -1;
	unsigned int	epflag = 0;

	if ( NULL == client || sockfd < 0 )
		return ret;

	if ( !client->inprocess ) {
		/* 不应该会执行到此处 */
		fprintf(stderr, "WRITE: client->inprocess equal 0 and sockfd equal %d\n",
				sockfd);
		return ret;
	}

	ret = http_client_response(client, &epflag);
	if (ret < 0)
		return ret;
	if ( HTTP_SOCKFD_OUT == epflag ) {
		return HTTP_SOCKFD_OUT;
	} else if ( HTTP_SOCKFD_IN == epflag ) {
		return HTTP_SOCKFD_IN;
	} else if ( HTTP_SOCKFD_DEL == epflag ) {
		return HTTP_SOCKFD_DEL;
	} else {
		/* 出错了 */
		return -1;
	}

	/* TODO */

	ret = http_client_keepalive(client);
	if ( ret && ret != -1 ) {
		/* 准备下一次连接请求 */
		http_client_reset(client);
		return HTTP_SOCKFD_IN;
	} else {
		return HTTP_SOCKFD_DEL;
	}
}
