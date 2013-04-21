#ifndef __HTTP__H
#define __HTTP__H

#include "mempool.h"

typedef enum {
	HTTP_09,	/* HTTP/0.9 */
	HTTP_10,	/* HTTP/1.0 */
	HTTP_11,	/* HTTP/1.1 */
	HTTP_UNKNOW
} http_protocol;

typedef enum{                 
	GET,                      
	HEAD,                     
	POST,                     
	PUT,                      
	DELETE,                   
	TRACE,                    
	CONNECT, 
	OPTIONS,                  
	UNKNOWN                   
} http_method;                  

typedef enum {
	Accept,                                                                      
	Accept_Charset,                                                              
	Accept_Encoding,                                                             
	Accept_Language,                                                             
	Accept_Range,                                                                
	Age,                                                                         
	Allow,                                                                       
	Authorization,                                                               
	Cache_Control,                                                               
	Connection,                                                                  
	Content_Encoding,                                                            
	Content_Language,                                                            
	Content_Length,                                                              
	Content_Location,                                                            
	Content_MD5,                                                                 
	Content_Range,   // eg: the first 500 bytes Content-Range: bytes 0-499/1234  
	Content_Type,                                                                
	Cookie,
	Date,                                                                        
	Etag,                                                                        
	Expect,                                                                      
	Expires,                                                                     
	From,                                                                        
	Host,                                                                        
	If_Match,                                                                    
	If_Modified_Since,                                                           
	If_None_Match,                                                               
	If_Range,                                                                    
	If_Unmodified_Since,                                                         
	Last_Modified,                                                               
	Location,                                                                    
	Max_Forwards,                                                                
	Pargma, //已过时了。                                                         
	Proxy_Authenticate,                                                          
	Proxy_Authorization,                                                         
	Range,                                                                       
	Referer,                                                                     
	Retry_After,                                                                 
	Server,                                                                      
	TE,                                                                          
	Trailer,                                     
	Transfer_Encoding,                           
	Upgrade,                                     
	User_Agent,                                  
	Vary,                                        
	Via,                                         
	Warning,                                     
	WWW_Authenticate,
	Head_All
}http_head_field;

struct head_field{
	/*
	struct list node;
	*/
	struct pool_entry *mem;
	int		valid;			/* 当前头域存在时为1, 不存在时为0 */
	http_head_field	hfd;
	void	*value; /* 当内容存在时指向的是值的开头 */
	/* 存放私有数据的指针 */
	void	*pri;
};


struct http_client {
	struct list node;
	/* 接收缓冲区，发送缓冲区，和额外缓冲区。
	 * 接受缓冲区存放输入数据(主要是头域), 如果接收的数据超过头域就启用
	 * 额外缓冲区, 发送缓冲区用于存放发送数据。
	 *
	 * 只有接受缓冲区在实例创建的时候申请，
	 * 其他两个缓冲区只在必要时申请.
	 */
	struct pool_entry *inbuff;
	struct pool_entry *extbuff;
	struct pool_entry *outbuff;

	http_protocol client_http_protocol;
	http_method client_method;
	char *uri;
	char *head;
	struct head_field fields[Head_All];

	/* 一次read 读取的字节大于头域的大小，部分或全部数据读取进来。
	 * content指向数据的开始， content_fst_size指示读入数据的大小。
	 */
	void	*content;
	int		content_fst_size;

	int		sockfd;
	int		keepalive; // 是否保存链接

	/* 标记http_client是否正在处理当中 */
	int		inprocess;
	int		head_valid; // 头域是否完整可用
};
	
#define  HTTP_MAX_HEAD_LEN 4095
#define  CRLF "\x0d\x0a"
#define  HTTP_CLIENT_MAX_FREE  1024 

#define  HTTP_SOCKFD_IN		0x01
#define  HTTP_SOCKFD_OUT	0x02
#define  HTTP_SOCKFD_DEL	0x04


int http_client_head_valid(struct http_client *client);
int http_client_req_entire(struct http_client *client);
int http_client_parse(struct http_client * client);
char * http_search_field(struct http_client *client, http_head_field field);
struct http_client *http_client_get(int sockfd);
void http_client_put(struct http_client *client);

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
int http_client_request(struct http_client * client, unsigned int *skflag);
int http_client_response(struct http_client *client, unsigned int *skflag);
#endif 
