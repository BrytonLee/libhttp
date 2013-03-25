#include "libhttp/http.h"
#include "process.h"

/*
 * 发送文件描述符到子进程
 */
int send_to_worker(int sfd, int sockfd)
{
	struct msghdr msg;
	struct iovec iov[1];

#ifdef HAVE_MSGHDR_MSG_CONTROL
	union {
		struct cmsghdr cm;
		char control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr *cmptr;

	if ( sockfd < 0 || sfd < 0 )
		return -1;

	msg.msg_control = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);

	cmptr = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_len = CMSG_LEN(sizeof(int));
	cmptr->cmsg_level = SOL_SOCKET; /* UNIX domain */
	cmptr->cmsg_type = SCM_RIGHTS; /* send/recvive file descriptor */
	*((int *)CMSG_DATA(cmptr)) == sockfd;
#else
	if ( sockfd < 0 || sfd < 0 )
		return -1;

	msg.msg_accrights = (caddr_t) &sockfd;
	msg.msg_accrightslen = sizeof(int);
#endif
	
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	
	//?? 发送0字节数据，不知道能否发送成功
	iov[0].iov_base = NULL;
	iov[0].iov_len = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	return (sendmsg(sfd, &msg, 0));
}

/* 
 * 从UNIX socket中接收文件描述符
 */
int recv_from_master(int rfd)
{
	struct msghdr msg;
	struct iovec iov[1];
	ssize_t n;
	int sockfd;

#ifdef HAVE_MSGHDR_MSG_CONTROL
	union {
		struct cmsghdr cm;
		char control[CMSG_SPACE(sizeof(int))];
	}control_un;
	struct cmsghdr *cmptr;

	msg.msg_control = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);
#else
	int newfd;
	msg.msg_accrights = (caddr_t) &newfd;
	msg.msg_accrightslen = sizeof(int);
#endif

	if ( rfd < 0 )
		return -1;
	
	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov[0].iov_base = NULL;
	iov[0].iov_len = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	if ( ( n = recvmsg(rfd, &msg, 0)) <= 0)
		return n;

#ifdef HAVE_MSGHDR_MSG_CONTROL
	if (( cmptr = CMSG_FIRSTHDR(&msg)) != NULL &&
			cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
		if (cmptr->cmsg_level != SOL_SOCKET ||
				cmptr->cmsg_type != SCM_RIGHTS)
			return -1;
		sockfd = *((int *)CMSG_DATA(cmptr));
	} else
		sockfd = -1;
#else
	if ( msg.msg_accrightslen == sizeof(int))
		sockfd = newfd;
	else
		sockfd = -1;
#endif
	return sockfd;
}

static __inline__ void remove_and_close(int epfd, int sockfd, struct epoll_event *ev)
{
	while ( (ret = epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, ev)) < 0 )
		if ( errno == EINTR )
			continue;
	while ( (close(sockfd) < 0))
		if ( errno == EINTR )
			continue;
}

/* 
 * worker 进程的工作函数，一直工作不退出 
 * 整个工作的核心代码实际上能够成为一个模型，
 * 对于外围的依赖主要是如何通过socket文件描述符取得描述符所对应的
 * 结构体(http client实例) 通过这个结构体在不同的时间点调用不同的
 * 处理函数，而处理的时间也是固定，把时间点上的处理函数替换成函数的指针
 * 即可做成通用模型。*/
void worker(int sv)
{
	int sockfd, epfd, nfds;
	struct epoll_event ev, evs[20];
	struct http_client * client = NULL;
	int rdsize = 0, wrsize= 0, i, ret;

	if ( sv < 0 ) {
		fprintf(stderr, "worker_process parameter error!\n");
		exit(-1);
	}

	epfd = epoll_create(256);
	if ( epfd < 0 ) {
		perror("epoll_create error:");
		exit(-1);
	}
	
	/* 使用默认的LT(level triggered)模式 */
	ev.data.fd = sv;
	ev.events = EPOLLIN;
	while ( (ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv, &ev) ) < 0 )
		if ( errno == EINTR )
			continue;

	while ( 1 ) {
		while ( ( nfds = epoll_wait(epfd, evs, 20, -1) ) < 0 ) {
			if ( errno == EINTR )
				continue;
		}

		for (i = 0; i < nfds; i++) {
			if( evs[i].data.fd == sv && evs[i].events & EPOLLIN ) {
				if ( (sockfd = recv_from_master(ev)) < 0 )
					continue;
				while ( ( ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv, &ev) ) < 0 )
					if ( errno == EINTR )
						continue;
					else 
						perror("epoll_ctl error:");
			} else if ( evs[i].events & EPOLLIN) {
				/* 
				 * socket可读，通过文件描述符来取得http client实例.
				 *
				 * 1. 如果没有取得说明这个socket是新连接需要处理http_head头部，
				 * 但对于一次read调用没有读取到完整http头部的连接，
				 * request_process将返回NULL,连接将被关闭。
				 *
				 * 2. 如果取到http client,且inprocess = 0, 视为一个新的http连接请求(keep-alive)。
				 *
				 * 3. 如果取到http client,且inprocess = 1, 说明有数据需要处理。
				 */
				client = http_client_get(evs[i].data.fd);
				if ( NULL == client || !client->inprocess ) {
					/* 新http连接请求 */
					rdsize = request_process(&client, epfd, evs[i].data.fd);
					if ( NULL == client ) {
						/* http request 处理出错 */
						remove_and_close(epfd, evs[i].data.fd, &ev);
					} else ( rdsize < 0 ) {
						/* 连接被关闭，或socket读出错 */
						http_free(client);
						remove_and_close(epfd, evs[i].data.fd, &ev);
					}
				} else {
					rdsize = data_process(client, epfd, evs[i].data.fd);
					if ( rdsize < 0 ) {
						/* 连接被关闭，或socket读出错 */
						http_free(client);
						remove_and_close(epfd, evs[i].data.fd, &ev);
				}
			} else if ( evs[i].events & EPOLLOUT ) {
				/* 有socket注册了监听写事件，并且目前socket可写 */
				client = http_client_get(evs[i].data.fd);
				if ( NULL == client ) {
				}
				wrsize = response_write_back(client, epfd, evs[i].data.fd, &ev);
				if ( wrsize < 0 ) {
					http_free(client);
					remove_and_close(epfd, evs[i].data.fd, &ev);
				}
			} else {
				/* ERROR */
				if ( evs[i].data.fd == sv ) {
					perror("sv error:");
					exit(0);
				} else
					remove_and_close(epfd, evs[i].data.fd, &ev);
			}
		} /* for (i = 0; i < nfds; i++) */
	} /* while ( 1 ) */
}
