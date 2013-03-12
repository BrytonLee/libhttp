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
	void	*pri;
};


struct http_client {
	struct head_field fields[Head_All];
	struct pool_entry *mem;
	http_protocol client_http_protocol;
	http_method client_method;
	char *uri;
	char *head;

	/* 一次read 读取的字节大于头域的大小，部分或全部数据读取进来。
	 * content指向数据的开始， content_fst_size指示读入数据的大小。
	 */
	void	*content;
	int		content_fst_size;
};
	
#define  HTTP_MAX_HEAD_LEN 4095
#define  CRLF "\x0d\x0a"

struct http_client *http_parse(struct pool_entry *data);
int http_free(struct http_client *client);
char * http_search_field(struct http_client *client, http_head_field field);
#endif 
