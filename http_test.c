#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include "libhttp/mempool.h"
#include "libhttp/http.h"

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

static void mem_dump(int sig)
{

	mem_pool_dump();
}

static void request_process(int sockfd)
{
	struct pool_entry *entry;
	struct http_client *client;
	char * value;
	int ret;

	if ( sockfd < -1 )
		return;
	
	ret = mem_pool_entry_get(&entry, HTTP_MAX_HEAD_LEN + 1);
	if ( -1 == ret )
		return;

	while ( 1 ) {
		ret = read(sockfd, entry->buff, HTTP_MAX_HEAD_LEN);
		if ( -1 == ret  && errno == EINTR ) 
			continue;
		else if ( -1 == ret ) {
			perror("read error: ");
			break;
		}
		entry->inuse_size = ret;
		*((char *)entry->buff + ret + 1) = '\0';
		//printf("%s\n", (char *)entry->buff);
		client = http_parse(entry);
		if ( NULL == client ) {
			mem_pool_entry_put(entry);
			return;
		}

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

		http_free(client);
		break;
	}
	mem_pool_entry_put(entry);
}

int main(int argc, char ** argv)
{
	int listenfd, sockfd;
	struct sockaddr_in serv_addr;
	struct sockaddr client_addr;
	int addr_len;
	int ret = -1;
	
	signal(SIGUSR1, mem_dump);
	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if ( -1 == listenfd ) {
		perror("Cannot create listen socket: ");
		return ret;
	}

	addr_len = sizeof(serv_addr);
	memset(&serv_addr, '\0', addr_len);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(8080);
	ret = bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr));
	if ( -1 == ret ) {
		perror("Cannot bind listen socket: ");
		return ret;
	}

	ret = listen(listenfd, 1024);
	if ( -1 == ret ) {
		perror("Cannot listen to the listen socket: ");
		return ret;
	}

	do {
		memset(&client_addr, '\0', addr_len);
		sockfd = accept(listenfd, (struct sockaddr *)&client_addr, &addr_len);
		if ( -1 == sockfd  && errno == EINTR ) 
			continue;
		else if ( -1 == sockfd ) {
			perror("accept error: ");
			return ret;
		}

		request_process(sockfd);

		while ( 1 ) {
			ret = close(sockfd);
			if ( -1 == ret && errno == EINTR )
				continue;
			else if ( -1 == ret ) { 
				perror("close error: ");
			}
			break;
		}
	} while (1);

	return 0;
}
