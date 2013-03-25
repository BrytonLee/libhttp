#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include "process.h"

static char *method_str[] = {
	"get",
	"head",
	"post",
	"put",
	"delete",
	"trace",
	"connect",
	"options"
};

static char *proto_str[] = {
	"HTTP/0.9",
	"HTTP/1.0",
	"HTTP/1.1"
};

static char *response_head = "HTTP/1.1 200 OK" CRLF
		"Content-Type: text/html;charset=UTF-8" CRLF
		"Content-Length: 93" CRLF
		"Connection: Keep-Alive" CRLF
		CRLF;

static char *index_htm="<html><head><title>Comming soon...</title></head>"
		"<body><h1>Comming soon...</h1></body></html>";

static void request_test(struct http_client *client)
{
	char * value;

	if ( NULL == client )
		return;

	/* METHOD URI PROTOCOL  HOST COOKIE */
	printf("method: %s\nURI: %s\nprotocol: %s\n",
			method_str[client->client_method],
			client->uri,
			proto_str[client->client_http_protocol]);
	if ( (value = http_search_field(client, Host)) )
		printf("Host: %s\n", value);
	else
		printf("Host: not found\n");

	if ( (value = http_search_field(client, Cookie)) )
		printf("Cookie: %s\n", value);
	else
		printf("Cookie: not found\n");

	printf("------------------\n");

}

static int response_test(struct http_client *client, int sockfd)
{
	int ret1, ret2;
	
	while ( (ret1 = write(sockfd, response_head, strlen(response_head))) < 0)
		if (errno == EINTR)
			continue;
	while ( (ret2 = write(sockfd, index_htm, strlen(index_htm))) < 0)
		if (errno == EINTR)
			continue;

	return ret1+ret2;
}


/*
 * request_process: 处理http连接请求。
 * 参数: client: client实例的指针,
 *		epfd: epoll实例文件描述符,
 *		sockfd: socket 文件描述符.
 * 返回: read的字节数： -1 表示出错，
 *		0 表示没有数据可读，也表示对端关闭了连接。
 *		client: 指向http client实例的指针。
 */
int request_process(struct http_client *client, int epfd, int sockfd, struct epoll_event *ev)
{
	struct pool_entry *entry;
	int ret = -1;

	if ( NULL == client || NULL == ev 
			|| epfd < -1 || sockfd < -1 )
		return ret;
	
	client->inprocess = 1;
	entry = client->mem;
	while ( 1 ) {
		ret = read(sockfd, entry->buff, HTTP_MAX_HEAD_LEN);
		if ( -1 == ret  && errno == EINTR ) 
			continue;
		else if ( -1 == ret ) {
			perror("read error: ");
			return ret;
		} else if ( 0 == ret ) {
			fprintf(stderr, "remote closed socket\n");
			return ret;
		}
		break;
	}

	entry->inuse_size = ret;
	*((char *)entry->buff + ret)  = '\0';
	if ( (http_client_parse(client)) < 0 )
		return -1;

	//request_test(client);

	/* 假设第一次read操作就把整个请求读取进来，后续没有数据*/
	{
		ev->data.fd = sockfd;
		ev->events = EPOLLOUT;
		epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, ev);
	}

	return ret;
}

int data_process(struct http_client *client, int epfd, int sockfd, struct epoll_event *ev)
{
	char buff[1024];
	int ret = -1;

	if ( NULL == client || epfd < 0 
			|| sockfd < 0 || NULL == ev)
		return ret;

	/* 只是简单的把数据丢掉*/
	do {
		while ( (ret = read(sockfd, buff, 1023)) < 0) 
			if ( errno == EINTR )
				continue;
	}while (ret == 0);
		
	ev->data.fd = sockfd;
	ev->events = EPOLLOUT;
	epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, ev);
	
	return 0;
}
int response_write_back(struct http_client *client, int epfd, int sockfd, struct epoll_event *ev)
{
	struct pool_entry *entry;
	int ret = -1;

	if ( NULL == client || epfd < 0 
			|| sockfd < 0 || NULL == ev)
		return ret;

	ret = response_test(client, sockfd);
	if (ret < 0)
		return ret;

	if ( client->keepalive ) {
		/* 准备下一次连接请求 */
		entry = client->mem;
		entry->inuse_size = 0;
		client->inprocess = 0;
		client->uri = client->head = NULL;
		client->content = NULL;
		client->content_fst_size = 0;

		ev->data.fd = sockfd;
		ev->events = EPOLLIN;
		epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, ev);
	} else {
		epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, ev);
		http_client_put(client);
		while ( (close(sockfd)) < 0 )
			if ( errno == EINTR )
				continue;
	}
		
	return ret;
}
