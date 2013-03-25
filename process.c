
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

/*
 * request_process: 处理http连接请求。
 * 参数: client是值-结果型参数,
 *		epfd: epoll实例文件描述符,
 *		sockfd: socket 文件描述符.
 * 返回: read的字节数： -1 表示出错，
 *		0 表示没有数据可读，也表示对端关闭了连接。
 *		client: 指向http client实例的指针。
 */
int request_process(struct http_client **client, int epfd, int sockfd)
{
	struct pool_entry *entry;
	char * value;
	int ret = -1;

	if ( NULL == client || epfd < -1 || sockfd < -1 )
		return ret;
	
	ret = mem_pool_entry_get(&entry, HTTP_MAX_HEAD_LEN + 1);
	if ( -1 == ret )
		return ret;

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
	*((char *)entry->buff + ret + 1) = '\0';
	if ( NULL == *client ) {
		*client = http_parse(entry);
		if ( NULL == *client ) {
			mem_pool_entry_put(entry);
			return -1;
		}
	} else {
		/* TODO 复用http client实例 */
	}
	*client->sockfd = sockfd;

	/* METHOD URI PROTOCOL  HOST COOKIE */
	printf("method: %s\nURI: %s\nprotocol: %s\n",
			method_str[*client->client_method],
			client->uri,
			proto_str[*client->client_http_protocol]);
	if ( (value = http_search_field(*client, Host)) )
		printf("Host: %s\n", value);
	else
		printf("Host: not found\n");

	if ( (value = http_search_field(*client, Cookie)) )
		printf("Cookie: %s\n", value);
	else
		printf("Cookie: not found\n");

	printf("------------------\n");

	return ret;
}
