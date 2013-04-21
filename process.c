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
			if ( ret == 1 )
				/* TODO 读取没有读取到请求完整的 content 
				 * 这里是出口之一*/
				return HTTP_SOCKFD_OUT;
			else if ( ret == 0) 
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

#if 0
int data_process(struct http_client *client, int epfd, int sockfd, struct epoll_event *ev)
{
#if 0
	char buff[1024];
#endif 
	struct pool_entry *mem;
	int	mem_insue = 0, redsize;
	int ret = -1;

	if ( NULL == client || epfd < 0 
			|| sockfd < 0 || NULL == ev)
		return ret;
	
	mem = client->inbuff;
	if ( mem->inuse_size ) {
		mem_insue = mem->inuse_size;
		ret = http_client_head_valid(client);
		if ( ret == 1 )
			/* TODO 读取没有读取到请求完整的 content */
			;
		else if ( ret == 0 ) {
			/* 还没读到完整的头域 */
			redsize = 0;
			redsize += mem_inuse;
			while ( 1 ) {
				ret = read(sockfd, (mem->buff + redsize),
						HTTP_MAX_HEAD_LEN - redsize);
				if ( ret == -1 && errno == EINTR )
					continue;
				else if ( ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK )){
					break;
				} 
				
				redsize += ret;
			}

			mem->inuse_size = redsize;
			*((char *)mem->buff + redsize) = '\0';
			ret = http_client_head_valid(client);
			if ( ret == 0 || ret == -1 ) {
				/* 如果在这个函数中还不能读取到完整的头域的链接全部丢掉 */
				return -1;
			} else {
				if ( (http_client_parse(client)) < 0 )
					return -1;

				/* 判断是否读取完整个请求 */
				ret = http_client_req_entire(client);
				if ( ret == 1 ) {
					// debug
					request_test(client);

					/* TODO: 实际这时的sockfd是可写的，socket的buffer为空
					 * 可以直接调用response函数, 写不成功了再挂epoll */
					/*
					ret = response_write_back(client, epfd, sockfd, ev)
					if (ret < 0)
						;
					*/
					ev->data.fd = sockfd;
					ev->events = EPOLLOUT;
					epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, ev);
				} else {
					/* TODO 读取请求的 content */
				}
			}
		}else 
			/* error */
			return ret;
	} else {
		/* error */
	}

#if 0

	/* 只是简单的把数据丢掉*/
	do {
		while ( (ret = read(sockfd, buff, 1023)) < 0) 
			if ( errno == EINTR )
				continue;
	}while (ret == 0);
		
	ev->data.fd = sockfd;
	ev->events = EPOLLOUT;
	epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, ev);
#endif
	
	return 0;
}
#endif 

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

	if ( client->keepalive ) {
		/* 准备下一次连接请求 */
		entry = client->inbuff;
		entry->inuse_size = 0;
		client->inprocess = 0;
		client->uri = client->head = NULL;
		client->content = NULL;
		client->content_fst_size = 0;

		return HTTP_SOCKFD_IN;
	} else {
		return HTTP_SOCKFD_DEL;
	}
}
