#include <stdlib.h>
#include <string.h>
#include "http.h"

static struct http_client * http_new_client()
{
	int size;
	struct http_client *client;

	size = sizeof(struct http_client);
	client = (struct http_client *)malloc(size);
	if ( NULL == client )
		return NULL;
	client->content = NULL;
	client->content_fst_size = 0;
	return client;
}

static void http_client_free(struct http_client *client)
{
	if ( NULL == client )
		return;
	
	free(client);
}

static int client_basic_info(struct http_client *client)
{
	struct pool_entry *data;
	char *buff, *tmp;
	int len, ret = -1;

	if ( NULL == client || NULL == client->mem )
		return ret;
	
	data = client->mem;
	if ( data->inuse_size < 0 )
		return ret;
	buff = data->buff;
	tmp = strchr(buff, ' ');
	if ( NULL == tmp )
		return ret;
	len = tmp-buff;
	switch ( len ) {
		case 3:
			/* GET/PUT */
			if ( !strncmp(buff, "GET", 3) )
				client->client_method = GET;
			else if ( !strncmp(buff, "PUT", 3) )
				client->client_method = PUT;
			else
				client->client_method = UNKNOWN;
			break;
		case 4:
			/* POST/HEAD */
			if ( !strncmp(buff, "POST", 4) )
				client->client_method = POST;
			else if ( !strncmp(buff, "HEAD", 4) )
				client->client_method = HEAD;
			else 
				client->client_method = UNKNOWN;
			break;
		case 5:
			/* TRACE */
			if ( !strncmp(buff, "TRACE", 5) )
				client->client_method = TRACE;
			else
				client->client_method = UNKNOWN;
			break;
		case 6:
			/* DELETE */
			if ( !strncmp(buff, "DELETE", 6) )
				client->client_method = DELETE;
			else
				client->client_method = UNKNOWN;
			break;
		case 7:
			/* CONNECT, OPTIONS */
			if ( !strncmp(buff, "CONNECT", 7) )
				client->client_method = CONNECT;
			else if ( !strncmp(buff, "OPTIONS", 7) )
				client->client_method = OPTIONS;
			else 
				client->client_method = UNKNOWN;
		default:
			/* UNKNOWN */
			client->client_method = UNKNOWN;
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
	field->mem = client->mem;
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
				if ( !strncmp(head, "Accept", 6) ) {
					field = &client->fields[Accept];
					http_head_field_init(field, client, Accept, head + 7);
				} else if ( !strncmp(head, "Accept_Charset", 14) ) {
					field = &client->fields[Accept_Charset];
					http_head_field_init(field, client, Accept_Charset, head + 15);
				} else if ( !strncmp(head, "Accept_Encoding", 15) ) {
					field = &client->fields[Accept_Encoding];
					http_head_field_init(field, client, Accept_Encoding, head + 16);
				} else if ( !strncmp(head, "Accept_Language", 15) ) {
					field = &client->fields[Accept_Language];
					http_head_field_init(field, client, Accept_Language, head + 16);
				} else if ( !strncmp(head, "Accept_Range", 12) ) {
					field = &client->fields[Accept_Range];
					http_head_field_init(field, client, Accept_Range, head + 13);
				} else if ( !strncmp(head, "Age", 3) ) {
					field = &client->fields[Age];
					http_head_field_init(field, client, Age, head + 4);
				} else if ( !strncmp(head, "Allow", 5) ) {
					field = &client->fields[Allow];
					http_head_field_init(field, client, Allow, head + 6);
				} else if ( !strncmp(head, "Authorization", 13) ) {
					field = &client->fields[Authorization];
					http_head_field_init(field, client, Authorization, head + 14);
				}
				break;
			case 'C':
				if ( !strncmp(head, "Cache_Control", 13) ) {
					field = &client->fields[Cache_Control];
					http_head_field_init(field, client, Cache_Control, head + 14);
				} else if ( !strncmp(head, "Connection", 10) ) {
					field = &client->fields[Connection];
					http_head_field_init(field, client, Connection, head + 11);
				} else if ( !strncmp(head, "Content_Encoding", 16) ) {
					field = &client->fields[Content_Encoding];
					http_head_field_init(field, client, Content_Encoding, head + 17);
				} else if ( !strncmp(head, "Content_Language", 16) ) { 
					field = &client->fields[Content_Language];
					http_head_field_init(field, client, Content_Language, head + 17);
				} else if ( !strncmp(head, "Content_Length", 14) ) {
					field = &client->fields[Content_Length];
					http_head_field_init(field, client, Content_Length, head + 15);
				} else if ( !strncmp(head, "Content_Location", 16) ) {
					field = &client->fields[Content_Location];
					http_head_field_init(field, client, Content_Location, head + 17);
				} else if ( !strncmp(head, "Content_MD5", 11) ) { 
					field = &client->fields[Content_MD5];
					http_head_field_init(field, client, Content_MD5, head + 12);
				} else if ( !strncmp(head, "Content_Range", 13) ) {
					field = &client->fields[Content_Range];
					http_head_field_init(field, client, Content_Range, head + 14);
				} else if ( !strncmp(head, "Content_Type", 12) ) {
					field = &client->fields[Content_Type];
					http_head_field_init(field, client, Content_Type, head + 13);
				}
				break;
			case 'D':
				if ( !strncmp(head, "Date", 4) ) {
					field = &client->fields[Date];
					http_head_field_init(field, client, Date, head + 5);
				}
				break;
			case 'E':
				if ( !strncmp(head, "Etag", 4) ) { 
					field = &client->fields[Etag];
					http_head_field_init(field, client, Etag, head + 5);
				} else if ( !strncmp(head, "Expect", 6) ) {
					field = &client->fields[Expect];
					http_head_field_init(field, client, Expect, head + 7);
				} else if ( !strncmp(head, "Expires", 7) ) {
					field = &client->fields[Expires];
					http_head_field_init(field, client, Expires, head + 8);
				}
				break;
			case 'F':
				if ( !strncmp(head, "From", 4) ) {
					field = &client->fields[From];
					http_head_field_init(field, client, From, head + 5);
				}
				break;
			case 'H':
				if ( !strncmp(head, "Host", 4) ) {
					field = &client->fields[Host];
					http_head_field_init(field, client, Host, head + 5);
				}
				break;
			case 'I':
				if ( !strncmp(head, "If_Match", 8) ) {
					field = &client->fields[If_Match];
					http_head_field_init(field, client, If_Match, head + 9);
				} else if ( !strncmp(head, "If_Modified_Since", 17) ) {
					field = &client->fields[If_Modified_Since];
					http_head_field_init(field, client, If_Modified_Since, head + 18);
				} else if ( !strncmp(head, "If_None_Match", 13) ) {
					field = &client->fields[If_None_Match];
					http_head_field_init(field, client, If_None_Match, head + 14);
				} else if ( !strncmp(head, "If_Range", 8) ) {
					field = &client->fields[If_Range];
					http_head_field_init(field, client, If_Range, head + 9);
				} else if ( !strncmp(head, "If_Unmodified_Since", 19) ) { 
					field = &client->fields[If_Unmodified_Since];
					http_head_field_init(field, client, If_Unmodified_Since, head + 20);
				}
				break;
			case 'L':
				if ( !strncmp(head, "Last_Modified", 13) ) {
					field = &client->fields[Last_Modified];
					http_head_field_init(field, client, Last_Modified, head + 14);
				} else if ( !strncmp(head, "Location", 8) ) {
					field = &client->fields[Location];
					http_head_field_init(field, client, Location, head + 9);
				}
				break;
			case 'M':
				if ( !strncmp(head, "Max_Forwards", 12) ) { 
					field = &client->fields[Max_Forwards];
					http_head_field_init(field, client, Max_Forwards, head + 13);
				}
				break;
			case 'P':
				if ( !strncmp(head, "Pargma", 6) ) {
					/* Pargma 已过时 */
					
					/* make gcc happy */
					head = head;
				} else if ( !strncmp(head, "Proxy_Authenticate", 18) ) {
					field = &client->fields[Proxy_Authenticate];
					http_head_field_init(field, client, Proxy_Authenticate, head + 19);
				} else if ( !strncmp(head, "Proxy_Authorization", 19) ) {
					field = &client->fields[Proxy_Authorization];
					http_head_field_init(field, client, Proxy_Authorization, head + 20);
				}
				break;
			case 'R':
				if ( !strncmp(head, "Range", 5) ) {
					field = &client->fields[Range];
					http_head_field_init(field, client, Range, head + 6);
				} else if ( !strncmp(head, "Referer", 7) ) {
					field = &client->fields[Referer];
					http_head_field_init(field, client, Referer, head + 8);
				} else if ( !strncmp(head, "Retry_After", 11 ) ) {
					field = &client->fields[Retry_After];
					http_head_field_init(field, client, Retry_After, head + 12);
				}
				break;
			case 'S':
				if ( !strncmp(head, "Server", 6) ) {
					field = &client->fields[Server];
					http_head_field_init(field, client, Server, head + 7);
				}
				break;
			case 'T':
				if ( !strncmp(head, "TE", 2) ) {
					field = &client->fields[TE];
					http_head_field_init(field, client, TE, head + 3);
				} else if ( !strncmp(head, "Trailer", 7) ) { 
					field = &client->fields[Trailer];
					http_head_field_init(field, client, Trailer, head + 8);
				} else if ( !strncmp(head, "Transfer_Encoding", 17) ) {
					field = &client->fields[Transfer_Encoding];
					http_head_field_init(field, client, Transfer_Encoding, head + 18);
				}
				break;
			case 'U':
				if ( !strncmp(head, "Upgrade", 7) ) {
					field = &client->fields[Upgrade];
					http_head_field_init(field, client, Upgrade, head + 8);
				} else if ( !strncmp(head, "User_Agent", 10) ) {
					field = &client->fields[User_Agent];
					http_head_field_init(field, client, User_Agent, head + 11);
				}
				break;
			case 'V':
				if ( !strncmp(head, "Vary", 4) ) {
					field = &client->fields[Vary];
					http_head_field_init(field, client, Vary, head + 5);
				} else if ( !strncmp(head, "Via", 3) ) {
					field = &client->fields[Via];
					http_head_field_init(field, client, Via, head + 4);
				}
				break;
			case 'W':
				if ( !strncmp(head, "Warning", 7) ) {
					field = &client->fields[Warning];
					http_head_field_init(field, client, Warning, head + 8);
				} else if ( !strncmp(head, "WWW_Authenticate", 16) ) {
					field = &client->fields[WWW_Authenticate];
					http_head_field_init(field, client, WWW_Authenticate, head + 17);
				}
				break;
			case '\x0d':
				if ( *(head + 1) == '\x0a' ) {
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

static int __http_parse(struct http_client *client)
{
	int ret = -1;

	if ( NULL == client || NULL == client->mem )
		return ret;

	if ( client_basic_info(client) )
		return ret;
	if ( client_head_field(client) )
		return ret;
	ret = 0;
	return ret;
}

struct http_client *http_parse(struct pool_entry *data)
{
	struct http_client *client;
	
	if ( NULL == data )
		return NULL;

	client = http_new_client();
	if ( NULL == client )
		return NULL;

	client->mem = data;
	if ( __http_parse(client)) {
		http_client_free(client);
	}
	return client;
}

int http_free(struct http_client *client)
{
	int ret = -1;

	if ( NULL == client || NULL != client->mem )
		return ret;

	http_client_free(client);
	ret = 0;
	return ret;
}

char * http_search_field(struct http_client *client, http_head_field field)
{
	if ( NULL == client || field < Accept || field > Head_All )
		return NULL;
	
	if ( client->fields[field].valid )
		return  client->fields[field].value;
	return NULL;
}
