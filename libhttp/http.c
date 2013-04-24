#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "http.h"

static struct list * hc_head = NULL, *hc_tail=NULL;
static int hc_size = 0,  hc_free = 0;

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
		"<body><h1>Comming soon...</h1></body></html>\n";

static int http_client_free(struct http_client *client);

__inline__ void http_client_reset(struct http_client *client)
{
	struct pool_entry *entry;
	int ret = -1;

	client->head_valid = 0;
	client->uri = client->head = NULL;
	client->content = NULL;
	client->content_fst_size = client->inprocess = 0;

	entry = client->inbuff;
	entry->inuse_size = 0;

	ret = http_client_keepalive(client);
	if ( !ret ) {
		client->sockfd = -1;
		client->keepalive = 0;
	}
}

static struct http_client * http_client_new(int sockfd, int mem_size)
{
	struct http_client * client = NULL;
	struct list *node = NULL;
	int ret;

	if ( mem_size < 0)
		return client;

	client = (struct http_client *)malloc(sizeof(struct http_client));
	if ( NULL == client ) {
		fprintf(stderr, "memory allocate failed!\n");
		return client;
	}
	memset(client, '\0', sizeof(struct http_client));
	ret = mem_pool_entry_get(&client->inbuff, mem_size);
	if ( 0 != ret || !client->inbuff ) {
		http_client_free(client);
		return client;
	}
	node = &client->node;
	node->pre = node->next = NULL;
	node->data = client;
	if ( !hc_size ) {
		hc_head = hc_tail = node;
	} else { 
		hc_tail->next = node;
		node->pre = hc_tail;
		hc_tail = node;
	}
	client->sockfd = sockfd;
	client->inprocess = client->keepalive = 0;

	hc_size++;
	hc_free++;
	return client;
}

static int http_client_free(struct http_client *client)
{
	struct list *pre, *next;

	if ( NULL == client )
		return -1;
	
	pre = client->node.pre;
	next = client->node.next;

	if ( pre )
		pre->next = next;
	else
		hc_head = next;
		
	if ( next )
		next->pre = pre;
	else
		hc_tail = pre;

	if ( client->inbuff )
		mem_pool_entry_put(client->inbuff);
	free(client);
	client = NULL;

	hc_size--;
	return 0;
}

static int client_basic_info(struct http_client *client)
{
	struct pool_entry *data;
	char *buff, *tmp;
	int len, ret = -1;

	if ( NULL == client || NULL == client->inbuff )
		return ret;
	
	data = client->inbuff;
	if ( data->inuse_size < 0 )
		return ret;
	buff = data->buff;
	tmp = strchr(buff, ' ');
	if ( NULL == tmp )
		return ret;

	client->client_method = UNKNOWN;
	len = tmp-buff;
	switch ( len ) {
		case 3:
			/* GET/PUT */
			if ( !strncmp(buff, "GET", 3) )
				client->client_method = GET;
			else if ( !strncmp(buff, "PUT", 3) )
				client->client_method = PUT;
			break;
		case 4:
			/* POST/HEAD */
			if ( !strncmp(buff, "POST", 4) )
				client->client_method = POST;
			else if ( !strncmp(buff, "HEAD", 4) )
				client->client_method = HEAD;
			break;
		case 5:
			/* TRACE */
			if ( !strncmp(buff, "TRACE", 5) )
				client->client_method = TRACE;
			break;
		case 6:
			/* DELETE */
			if ( !strncmp(buff, "DELETE", 6) )
				client->client_method = DELETE;
			break;
		case 7:
			/* CONNECT, OPTIONS */
			if ( !strncmp(buff, "CONNECT", 7) )
				client->client_method = CONNECT;
			else if ( !strncmp(buff, "OPTIONS", 7) )
				client->client_method = OPTIONS;
		default:
			/* UNKNOWN */
			break;
	}

	/* URI */
	buff = ++tmp;
	tmp = strchr(buff, ' ');
	if ( NULL == tmp )
		return ret;
	len = tmp - buff;
	/* URI 的长度协议没有规定大小，1024上限问题不大 */
	if ( len < 0 || len > 1024 )
		return ret;
	*tmp = '\0';
	/* TODO: uri容易出现安全问题，应该做安全过滤. */
	client->uri = buff;

	/* protocol */
	buff = ++tmp;
	tmp = strstr(buff, CRLF);
	if ( NULL == tmp )
		return ret;
	if ( !strncmp(buff, "HTTP/0.9", 8) )
		client->client_http_protocol = HTTP_09;
	else if ( !strncmp(buff, "HTTP/1.0", 8) )
		client->client_http_protocol = HTTP_10;
	else if ( !strncmp(buff, "HTTP/1.1", 8) )
		client->client_http_protocol = HTTP_11;
	else
		return ret;
	
	client->head = tmp + 2;
	ret = 0;
	return ret;
}

static void check_valid(struct head_field *field)
{
	
	if ( strlen(field->value) > 0 ) 
		field->valid = 1;
	else
		field->valid = 0;
}

static __inline__ void http_head_field_init(struct head_field * field, 
		struct http_client *client,	http_head_field hfd, void * value)
{
	field->mem = client->inbuff;
	field->hfd = hfd;
	field->value = value;
	check_valid(field);
	field->pri = NULL;
}

static int client_head_field(struct http_client *client)
{
	char *head, *tmp;
	struct head_field *field;
	int ret = -1;

	if ( NULL == client || NULL == client->head ) 
		return  ret;
	
	for ( head = client->head; (tmp = strstr(head, CRLF)); head = tmp){
		*tmp = '\0';
		tmp += 2;
		switch ( *head ) {
			case 'A':
				if ( !strncmp(head, "Accept:", 7) ) {
					field = &client->fields[Accept];
					http_head_field_init(field, client, Accept, head + 8);
				} else if ( !strncmp(head, "Accept_Charset:", 15) ) {
					field = &client->fields[Accept_Charset];
					http_head_field_init(field, client, Accept_Charset, head + 16);
				} else if ( !strncmp(head, "Accept_Encoding:", 16) ) {
					field = &client->fields[Accept_Encoding];
					http_head_field_init(field, client, Accept_Encoding, head + 17);
				} else if ( !strncmp(head, "Accept_Language:", 16) ) {
					field = &client->fields[Accept_Language];
					http_head_field_init(field, client, Accept_Language, head + 17);
				} else if ( !strncmp(head, "Accept_Range:", 13) ) {
					field = &client->fields[Accept_Range];
					http_head_field_init(field, client, Accept_Range, head + 14);
				} else if ( !strncmp(head, "Age:", 4) ) {
					field = &client->fields[Age];
					http_head_field_init(field, client, Age, head + 5);
				} else if ( !strncmp(head, "Allow:", 6) ) {
					field = &client->fields[Allow];
					http_head_field_init(field, client, Allow, head + 7);
				} else if ( !strncmp(head, "Authorization:", 14) ) {
					field = &client->fields[Authorization];
					http_head_field_init(field, client, Authorization, head + 15);
				}
				break;
			case 'C':
				if ( !strncmp(head, "Cache_Control:", 14) ) {
					field = &client->fields[Cache_Control];
					http_head_field_init(field, client, Cache_Control, head + 15);
				} else if ( !strncmp(head, "Connection:", 11) ) {
					field = &client->fields[Connection];
					http_head_field_init(field, client, Connection, head + 12);
				} else if ( !strncmp(head, "Content_Encoding:", 17) ) {
					field = &client->fields[Content_Encoding];
					http_head_field_init(field, client, Content_Encoding, head + 18);
				} else if ( !strncmp(head, "Content_Language:", 17) ) { 
					field = &client->fields[Content_Language];
					http_head_field_init(field, client, Content_Language, head + 18);
				} else if ( !strncmp(head, "Content_Length:", 15) ) {
					field = &client->fields[Content_Length];
					http_head_field_init(field, client, Content_Length, head + 16);
				} else if ( !strncmp(head, "Content_Location:", 17) ) {
					field = &client->fields[Content_Location];
					http_head_field_init(field, client, Content_Location, head + 18);
				} else if ( !strncmp(head, "Content_MD5:", 12) ) { 
					field = &client->fields[Content_MD5];
					http_head_field_init(field, client, Content_MD5, head + 13);
				} else if ( !strncmp(head, "Content_Range:", 14) ) {
					field = &client->fields[Content_Range];
					http_head_field_init(field, client, Content_Range, head + 15);
				} else if ( !strncmp(head, "Content_Type:", 13) ) {
					field = &client->fields[Content_Type];
					http_head_field_init(field, client, Content_Type, head + 14);
				}
				break;
			case 'D':
				if ( !strncmp(head, "Date:", 5) ) {
					field = &client->fields[Date];
					http_head_field_init(field, client, Date, head + 6);
				}
				break;
			case 'E':
				if ( !strncmp(head, "Etag:", 5) ) { 
					field = &client->fields[Etag];
					http_head_field_init(field, client, Etag, head + 6);
				} else if ( !strncmp(head, "Expect:", 7) ) {
					field = &client->fields[Expect];
					http_head_field_init(field, client, Expect, head + 8);
				} else if ( !strncmp(head, "Expires:", 8) ) {
					field = &client->fields[Expires];
					http_head_field_init(field, client, Expires, head + 9);
				}
				break;
			case 'F':
				if ( !strncmp(head, "From:", 5) ) {
					field = &client->fields[From];
					http_head_field_init(field, client, From, head + 6);
				}
				break;
			case 'H':
				if ( !strncmp(head, "Host:", 5) ) {
					field = &client->fields[Host];
					http_head_field_init(field, client, Host, head + 6);
				}
				break;
			case 'I':
				if ( !strncmp(head, "If_Match:", 9) ) {
					field = &client->fields[If_Match];
					http_head_field_init(field, client, If_Match, head + 10);
				} else if ( !strncmp(head, "If_Modified_Since:", 18) ) {
					field = &client->fields[If_Modified_Since];
					http_head_field_init(field, client, If_Modified_Since, head + 19);
				} else if ( !strncmp(head, "If_None_Match:", 14) ) {
					field = &client->fields[If_None_Match];
					http_head_field_init(field, client, If_None_Match, head + 15);
				} else if ( !strncmp(head, "If_Range:", 9) ) {
					field = &client->fields[If_Range];
					http_head_field_init(field, client, If_Range, head + 10);
				} else if ( !strncmp(head, "If_Unmodified_Since:", 20) ) { 
					field = &client->fields[If_Unmodified_Since];
					http_head_field_init(field, client, If_Unmodified_Since, head + 21);
				}
				break;
			case 'L':
				if ( !strncmp(head, "Last_Modified:", 14) ) {
					field = &client->fields[Last_Modified];
					http_head_field_init(field, client, Last_Modified, head + 15);
				} else if ( !strncmp(head, "Location:", 9) ) {
					field = &client->fields[Location];
					http_head_field_init(field, client, Location, head + 10);
				}
				break;
			case 'M':
				if ( !strncmp(head, "Max_Forwards:", 13) ) { 
					field = &client->fields[Max_Forwards];
					http_head_field_init(field, client, Max_Forwards, head + 14);
				}
				break;
			case 'P':
				if ( !strncmp(head, "Pargma:", 7) ) {
					/* Pargma 已过时 */
					
					/* make gcc happy */
					head = head;
				} else if ( !strncmp(head, "Proxy_Authenticate:", 19) ) {
					field = &client->fields[Proxy_Authenticate];
					http_head_field_init(field, client, Proxy_Authenticate, head + 20);
				} else if ( !strncmp(head, "Proxy_Authorization:", 20) ) {
					field = &client->fields[Proxy_Authorization];
					http_head_field_init(field, client, Proxy_Authorization, head + 21);
				}
				break;
			case 'R':
				if ( !strncmp(head, "Range:", 6) ) {
					field = &client->fields[Range];
					http_head_field_init(field, client, Range, head + 7);
				} else if ( !strncmp(head, "Referer:", 8) ) {
					field = &client->fields[Referer];
					http_head_field_init(field, client, Referer, head + 9);
				} else if ( !strncmp(head, "Retry_After:", 12 ) ) {
					field = &client->fields[Retry_After];
					http_head_field_init(field, client, Retry_After, head + 13);
				}
				break;
			case 'S':
				if ( !strncmp(head, "Server:", 7) ) {
					field = &client->fields[Server];
					http_head_field_init(field, client, Server, head + 8);
				}
				break;
			case 'T':
				if ( !strncmp(head, "TE:", 3) ) {
					field = &client->fields[TE];
					http_head_field_init(field, client, TE, head + 4);
				} else if ( !strncmp(head, "Trailer:", 8) ) { 
					field = &client->fields[Trailer];
					http_head_field_init(field, client, Trailer, head + 9);
				} else if ( !strncmp(head, "Transfer_Encoding:", 18) ) {
					field = &client->fields[Transfer_Encoding];
					http_head_field_init(field, client, Transfer_Encoding, head + 19);
				}
				break;
			case 'U':
				if ( !strncmp(head, "Upgrade:", 8) ) {
					field = &client->fields[Upgrade];
					http_head_field_init(field, client, Upgrade, head + 9);
				} else if ( !strncmp(head, "User_Agent:", 11) ) {
					field = &client->fields[User_Agent];
					http_head_field_init(field, client, User_Agent, head + 12);
				}
				break;
			case 'V':
				if ( !strncmp(head, "Vary:", 5) ) {
					field = &client->fields[Vary];
					http_head_field_init(field, client, Vary, head + 6);
				} else if ( !strncmp(head, "Via:", 4) ) {
					field = &client->fields[Via];
					http_head_field_init(field, client, Via, head + 5);
				}
				break;
			case 'W':
				if ( !strncmp(head, "Warning:", 8) ) {
					field = &client->fields[Warning];
					http_head_field_init(field, client, Warning, head + 9);
				} else if ( !strncmp(head, "WWW_Authenticate:", 17) ) {
					field = &client->fields[WWW_Authenticate];
					http_head_field_init(field, client, WWW_Authenticate, head + 18);
				}
				break;
			case '\x0d':
				if ( *(head + 1) == '\x0a' ) {
					client->content = head + 2;
					client->content_fst_size = client->inbuff->inuse_size - 
						(client->content - client->inbuff->buff);
					ret = 0;
					return ret;
				}
				break;
			default:
				/* unknow head field */
				break;
		}
	}
}

/* 判断http head的可用性 */
int http_client_head_valid(struct http_client *client)
{
	struct pool_entry * mem;
	
	if ( NULL == client )
		return -1;
	
	if ( !client->head_valid ) {
		mem = client->inbuff;
		/* sizeof ("GET / HTTP/1.1\r\n") = 17 */
		if ( mem->inuse_size < 17 )
			return 0;
		else if ( strstr(mem->buff, "\r\n\r\n" ) ) {
			client->head_valid = 1;	
			return 1;
		}else if ( mem->inuse_size < (HTTP_MAX_HEAD_LEN - 4) )
			return 0;
		else
			return -1;
	} else 
		return client->head_valid;
}

/* 判断是否读取完整个请求 
 * 在http 解析过才能调用 */
int http_client_req_entire(struct http_client *client)
{
	struct pool_entry * mem;
	char *cnt_len_str;
	int	av_cnt_len = 0, cnt_len = 0;

	if ( NULL == client )
		return -1;

	/* 取出content_length */
	cnt_len_str = http_search_field(client, Content_Length);
	if ( NULL == cnt_len_str ) {
		/* 如果没有取到Content_Length，默认为完整的请求 */
		return 1;
	} else {
		cnt_len = atoi(cnt_len_str);
		if ( cnt_len == client->content_fst_size )
			return 1;
		else {
			mem = client->inbuff;
			av_cnt_len = (mem->buff + mem->inuse_size) - client->content;
			if ( cnt_len == av_cnt_len )
				return 1;
			else 
				return 0;
		}
	}
}

int http_client_parse(struct http_client * client)
{
	char *value;

	if ( NULL == client || NULL == client->inbuff )
		return -1;

	if ( client_basic_info(client) )
		return -1;
	if ( client_head_field(client) )
		return -1;

	/* keep-alive */
	if ( (value = http_search_field(client, Connection)) ) {
		if ( !(strncmp(value, "keep-alive", 10 )) )
			client->keepalive = 1;
	}
	return 0;
}


char * http_search_field(struct http_client *client, http_head_field field)
{
	if ( NULL == client || field < Accept || field > Head_All )
		return NULL;
	
	if ( client->fields[field].valid )
		return  client->fields[field].value;
	return NULL;
}

//#define HTTP_CLIENT_DEBUG
#ifdef HTTP_CLIENT_DEBUG
static __inline__ void http_client_dump()
{
	struct http_client *tmp;
	struct list *node = hc_head;
	int i;

	fprintf(stderr, "hc_size: %d, hc_free: %d\nhc_head: %p, hc_tail: %p\n", 
			hc_size, hc_free, hc_head, hc_tail);
	for ( i = 0; i < hc_size; i++ ) {
		tmp = (struct http_client *)node->data;
		fprintf(stderr, "#%d sockfd: %d, inprocess: %d\n", 
				i+1, tmp->sockfd, tmp->inprocess);
		node=node->next;
	}
}
#else 
#define http_client_dump()
#endif

/* 通过socket文件描述符取得http client实例.
 * NULL: 失败，没有找到;否则表示成功。*/
struct http_client *http_client_get(int sockfd)
{
	struct list *tmp = NULL;
	struct http_client *client = NULL;
	int inuse;

	if ( sockfd < 0 ) 
		return NULL;

	http_client_dump();
	
	if ( !hc_size )
		goto newclient;

	tmp = hc_tail;
	for ( inuse = hc_size - hc_free; inuse > 0; inuse--) {
		client = tmp->data;
		if ( client->sockfd == sockfd ) {
			/* debug */
			//fprintf(stderr, "%s hit! sockfd: %d\n", __func__, sockfd);
			return client;
		}
		tmp=tmp->pre;
	}

	if ( hc_free ) {
		client = hc_head->data;
		if ( hc_size > 1) {
			hc_head = hc_head->next;
			hc_head->pre = NULL;

			hc_tail->next = &client->node;
			client->node.pre = hc_tail;
			hc_tail = &client->node;
			hc_tail->next = NULL;
		}
		client->sockfd = sockfd;
		client->keepalive = client->inprocess = 0;
		hc_free--;
		return client;
	} 

newclient:
	/* 新建一个实例 */
	client = http_client_new(sockfd, HTTP_MAX_HEAD_LEN +1);
	if ( NULL == client )
		return NULL;
	hc_free--;
	return client;
}

void http_client_put(struct http_client *client)
{
	struct list *node, *pre, *next;

	if ( NULL == client )
		return;

	// debug 
	http_client_dump();
	if ( !client->inprocess ) {
		fprintf(stderr, "%s, hc_size: %d hc_free: %d client->inprocess: %d\n", __func__, 
				hc_size, hc_free, client->inprocess);
	}

	if ( hc_free > HTTP_CLIENT_MAX_FREE )
		http_client_free(client);
	else {
		node = &client->node;
		if ( node == hc_head ) {
			client->keepalive = 0;
			http_client_reset(client);
		} else if ( node == hc_tail ) {
			hc_tail = node->pre;
			hc_tail->next = NULL;

			node->pre = NULL;
			node->next = hc_head;
			hc_head->pre = node;
			hc_head = node;
			client->keepalive = 0;
			http_client_reset(client);
		} else {
			pre = node->pre;
			next = node->next;

			pre->next = next;
			next->pre = pre;
			
			node->pre = NULL;
			node->next = hc_head;
			hc_head->pre = node;
			hc_head = node;
			client->keepalive = 0;
			http_client_reset(client);
		}
		hc_free++;
	}	
}

/* 判断是否保持长链接 */
__inline int http_client_keepalive(struct http_client *client)
{
	if ( NULL == client )
		return -1;
	return client->keepalive;
}

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
		else if ( ret1 == -1 && (errno == EAGAIN || errno ==  EWOULDBLOCK ))
			break;

	while ( (ret2 = write(sockfd, index_htm, strlen(index_htm))) < 0)
		if (errno == EINTR)
			continue;
		else if ( ret2 == -1 && (errno == EAGAIN || errno ==  EWOULDBLOCK ))
			break;

	return ret1+ret2;
}

/* http_client_current_buff 
 * 当接收的数据超过(或可能超过)当前默认的接收缓冲区(inbuff)时，
 * 需要判断是活要用额外缓冲区(extbuff)。
 */
int http_client_current_buff(struct http_client *client, 
		struct pool_entry *mem)
{
	/* TODO */
	fprintf(stderr, "call %s\n", __func__);
}

/* 
 * 处理http 请求.
 * 参数:
 *	client	struct http_client实例
 *	*skflag	值结果型参数，函数执行之后socket要执行的动作。
 * 返回值:
 *	0		成功
 *	-1		失败
 *
 * 备注: 在http_client_head_valid 成功之后调用。
 */
int http_client_request(struct http_client *client, unsigned int *skflag)
{
	int ret = -1;

	if ( NULL == client || NULL == skflag)
		return -1;

	/* 判断是否读取完整个请求 */
	ret = http_client_req_entire(client);
	if ( ret == 1 ) {
		// debug
		//request_test(client);

		/* TODO: 实际这时的sockfd是可写的，socket的buffer为空
		 * 可以直接调用response函数, 写不成功了在挂epoll */
		/*
		ret = response_write_back(client, epfd, sockfd, ev)
		if (ret < 0)
			;
		*/
		*skflag = HTTP_SOCKFD_OUT;
	} else {
		/* TODO 根据不同的方法调用不同函数接受数据 */
		*skflag = HTTP_SOCKFD_IN;
	}

	return 0;
}

int http_client_response(struct http_client *client, unsigned int *skflag)
{
	int ret = -1;
	int sockfd;

	if ( NULL == client || NULL == skflag)
		return -1;

	sockfd  = client->sockfd;
	return response_test(client, sockfd);
}
