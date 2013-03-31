#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include "libhttp/http.h"
#include "worker.h"
#include "process.h"

/*
 * 发送文件描述符到子进程
 */
int send_to_worker(int sfd, int sockfd)
{
	struct msghdr msg;
	struct iovec iov[1];

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
	*((int *)CMSG_DATA(cmptr)) = sockfd;
	
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	
	char *strfd="fd";
	iov[0].iov_base = strfd;
	iov[0].iov_len = 2;
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
	char buff[2];
	ssize_t n;
	int sockfd;

	union {
		struct cmsghdr cm;
		char control[CMSG_SPACE(sizeof(int))];
	}control_un;
	struct cmsghdr *cmptr;

	msg.msg_control = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);

	if ( rfd < 0 )
		return -1;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov[0].iov_base = buff;
	iov[0].iov_len = 2;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	if ( ( n = recvmsg(rfd, &msg, 0)) <= 0) {
		if ( n < 0 )
			perror("recv_from_master:");
		return n;
	}

	if (( cmptr = CMSG_FIRSTHDR(&msg)) != NULL &&
			cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
		if (cmptr->cmsg_level != SOL_SOCKET ||
				cmptr->cmsg_type != SCM_RIGHTS)
			return -1;
		sockfd = *((int *)CMSG_DATA(cmptr));
	} else
		sockfd = -1;
	return sockfd;
}

static __inline__ void remove_and_close(int epfd, int sockfd, struct epoll_event *ev)
{
	int ret;

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
static void worker(int sv)
{
	pid_t pid;
	int sockfd, epfd, nfds;
	struct epoll_event ev, evs[20];
	struct http_client * client = NULL;
	int rdsize = 0, wrsize= 0, i, ret;


	if ( sv < 0 ) {
		fprintf(stderr, "worker_process parameter error!\n");
		exit(-1);
	}

	signal(SIGUSR1, mem_pool_dump);
	// signal(SIGTERM, xxx);
	
	pid = getpid();

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
				if ( (sockfd = recv_from_master(sv)) < 0 ) {
					fprintf(stderr, "recv_from_master error\n");
					continue;
				}
				ev.data.fd = sockfd;
				ev.events = EPOLLIN;
				while ( ( ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) ) < 0 )
					if ( errno == EINTR )
						continue;
					else 
						perror("epoll_ctl error:");
			} else if ( evs[i].events & EPOLLIN) {
				client = http_client_get(evs[i].data.fd);
				if ( !client->inprocess ) {
					/* 新http连接请求 */
					rdsize = request_process(client, epfd, evs[i].data.fd, &ev);
					if ( rdsize <= 0 ) {
						/* 连接被关闭，或socket读出错 */
						http_client_put(client);
						remove_and_close(epfd, evs[i].data.fd, &ev);
					}
				} else {
					rdsize = data_process(client, epfd, evs[i].data.fd, &ev);
					if ( rdsize < 0 ) {
						/* 连接被关闭，或socket读出错 */
						http_client_put(client);
						remove_and_close(epfd, evs[i].data.fd, &ev);
					}
				}
			} else if ( evs[i].events & EPOLLOUT ) {
				/* 有socket注册了监听写事件，并且目前socket可写 */
				client = http_client_get(evs[i].data.fd);
				if ( !client->inprocess ) {
					/* 不应该会执行到此处 */
					fprintf(stderr, "WRITE: client->inprocess equal 0 and sockfd equal %d\n",
							evs[i].data.fd);
					http_client_put(client);
					remove_and_close(epfd, evs[i].data.fd, &ev);
					continue;
				}
				wrsize = response_write_back(client, epfd, evs[i].data.fd, &ev);
				if ( wrsize < 0 ) {
					http_client_put(client);
					remove_and_close(epfd, evs[i].data.fd, &ev);
				}
			} else {
				/* ERROR */
				if ( evs[i].data.fd == sv ) {
					perror("sv error:");
					exit(0);
				} else {
					fprintf(stderr, "error happened on socket: %d\n",
							evs[i].data.fd);
					client = http_client_get(evs[i].data.fd);
					if ( client )
						http_client_put(client);
					remove_and_close(epfd, evs[i].data.fd, &ev);
				}
			}
		} /* for (i = 0; i < nfds; i++) */
	} /* while ( 1 ) */
}

/* create_worker:
 * 创建工作进程和unix domain socket pair.
 * 返回： 0: 成功。
 *		 -1: 失败。
 */
int create_worker(int worker_n, int worker_sv[][2], int closefd)
{
	pid_t pid[worker_n];
	int i,j;
	int ret = -1;

	i = 0;
	do { 
		ret = socketpair(AF_LOCAL, SOCK_STREAM, 0, worker_sv[i]);
		if ( -1 == ret ) {
			perror("socketpair error: ");
			for (j = i-1; j >= 0; j--) {
				kill(pid[j], SIGTERM);
				close(worker_sv[j][1]);
			}
			return ret;
		}

		pid[i] = fork();
		if ( pid[i] == 0 ) {
			close(closefd);
			for (j = i-1; j >= 0; j--) {
				close(worker_sv[j][1]);
			}
			worker(worker_sv[i][0]);
		} else if ( pid[i] < 0 ) {
			perror("fork error: ");
			for (j = i-1; j >= 0; j--) {
				kill(pid[j], SIGTERM);
				close(worker_sv[j][1]);
			};
			return ret;
		}
		close(worker_sv[i][0]);
		i++;
	} while ( i < worker_n );
	
	return 0;
}
