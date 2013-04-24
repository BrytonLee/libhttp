#define _GNU_SOURCE 
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include "libhttp/http.h"
#include "worker.h"
#include "process.h"
#ifdef __HTTP_ACCEPT_LOCK__
#include <semaphore.h>
#include <string.h>

sem_t	*accept_lock;
int listenfd;
int total_sockfd = 0;
#endif

static __inline__ void epoll_set_in(int epfd, int sockfd, struct epoll_event *ev)
{
	int ret;

	ev->data.fd = sockfd;
	ev->events = EPOLLIN;

	while ( (ret = epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, ev)) < 0 )
		if ( ret == -1 && errno == EINTR )
			continue;
		else {
			perror("epoll_ctl error: ");
			break;
		}
}

static __inline__ void epoll_set_out(int epfd, int sockfd, struct epoll_event *ev)
{
	int ret;

	ev->data.fd = sockfd;
	ev->events = EPOLLOUT;

	while ( (ret = epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, ev)) < 0 )
		if ( ret == -1 && errno == EINTR )
			continue;
		else {
			perror("epoll_ctl error: ");
			break;
		}
}

static __inline__ void epoll_skfd_del(int epfd, int sockfd, struct epoll_event *ev)
{
	int ret;
	
	while ( (ret = epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, ev)) < 0 )
		if ( errno == EINTR )
			continue;
		else {
			perror("epoll_ctl error: ");
			break;
		}

	while ( (close(sockfd) < 0))
		if ( errno == EINTR )
			continue;
#ifdef __HTTP_ACCEPT_LOCK__
	total_sockfd--;
#endif
}

/* 设置套接字非阻塞 */
int set_fd_nonblock(int sock)
{
	int flags;
	flags = fcntl(sock, F_GETFL, 0);
	if ( flags < 0 ) {
		perror("fcntl(F_GETFL) failed!");
		return -1;
	}

	if ( fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("fcntl(F_SETFL) failed!");
		return -1;
	}
	return 0;
}

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
#ifdef __HTTP_ACCEPT_LOCK__
	struct sockaddr client_addr;
	int addr_len;
	int lstd_on = 0; // 判断listenfd 是否在epoll的开关 
#endif


	if ( sv < 0 ) {
		fprintf(stderr, "worker_process parameter error!\n");
		exit(-1);
	}

	//signal(SIGUSR1, mem_pool_dump);
	//signal(SIGTERM, xxx);
	
	pid = getpid();

	epfd = epoll_create(256);
	if ( epfd < 0 ) {
		perror("epoll_create error:");
		exit(-1);
	}
	
	ret = set_fd_nonblock(sv);	
	if ( ret < 0) 
		exit(-1);

	/* 使用默认的LT(level triggered)模式 */
	memset(&ev, '\0', sizeof(ev));
	memset(evs, '\0', sizeof(evs));
	ev.data.fd = sv;
	ev.events = EPOLLIN;
	while ( (ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv, &ev) ) < 0 )
		if ( errno == EINTR )
			continue;
		else {
			perror("epoll_ctl error");
			exit(-1);
		}

	while ( 1 ) {
#ifdef __HTTP_ACCEPT_LOCK__
		if ( !lstd_on ) {
			/* 先取到锁，取到了锁的才有机会添加到epoll当中 */
			if ( total_sockfd ) {
				while ( 1 ) {
					ret = sem_trywait(accept_lock);
					if ( ret < 0 && errno == EINTR )
						continue;
					else if ( ret < 0 && errno == EAGAIN )
						break;
					else if ( ret < 0) {
						perror("sem_trywait error: ");
						exit(-1);
					}
					break;
				}
			} else {
				/* 不忙的时候挂sem_wait */
				while ( 1 ) {
					ret = sem_wait(accept_lock);
					if ( ret < 0 && errno == EINTR )
						continue;
					else if ( ret < 0 ) {
						perror("sem_trywait error: ");
						exit(-1);
					}
					break;
				}
			}

			if ( ret == 0 ) {
				/* 取到了锁，添加到epoll */
				ev.data.fd = listenfd;
				ev.events = EPOLLIN;
				while ( (ret = epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) ) < 0)
					if ( errno == EINTR )
						continue;
					else 
						perror("epoll_ctl add listen fd error: ");
				lstd_on = 1;
			}
		}
#endif			

		while ( ( nfds = epoll_wait(epfd, evs, 20, -1) ) < 0 ) {
			if ( errno == EINTR )
				continue;
		}

		for (i = 0; i < nfds; i++) {
#ifdef __HTTP_ACCEPT_LOCK__
			if ( evs[i].data.fd == listenfd && evs[i].events & EPOLLIN ) {
				/* 接收新链接 */
				addr_len = sizeof(client_addr);
				memset(&client_addr, '\0', addr_len);
				while ( 1 ) {
					sockfd = accept(listenfd, (struct sockaddr *)&client_addr, &addr_len);
					if ( -1 == sockfd && errno == EINTR ) 
						continue;
					else if ( -1 == sockfd ) {
						perror("accept error: ");
					}
					break;
				}

				if ( sockfd < 0 )
					continue;

				while ( ( ret = epoll_ctl(epfd, EPOLL_CTL_DEL, listenfd, &ev) ) < 0)
					if ( errno == EINTR )
						continue;
					else {
						fprintf(stderr, "Can not remove listenfd from epoll.\n");
						break;
					}

				lstd_on = 0;
				/* 解锁 */
				while ( 1 ) {
					ret = sem_post(accept_lock);
					if ( ret < 0 && errno == EINTR )
						continue;
					break;
				}

				set_fd_nonblock(sockfd);
				ev.data.fd = sockfd;
				ev.events = EPOLLIN;
				while ( ( ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) ) < 0 )
					if ( errno == EINTR )
						continue;
					else 
						perror("epoll_ctl error:");
				total_sockfd++;

				continue;
			} else
#endif
			if ( evs[i].data.fd == sv && evs[i].events & EPOLLIN ) {
#ifndef __HTTP_ACCEPT_LOCK__
				if ( (sockfd = recv_from_master(sv)) < 0 ) {
					fprintf(stderr, "recv_from_master error\n");
					continue;
				}
				set_fd_nonblock(sockfd);
				ev.data.fd = sockfd;
				ev.events = EPOLLIN;
				while ( ( ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) ) < 0 )
					if ( errno == EINTR )
						continue;
					else 
						perror("epoll_ctl error:");
#else
				fprintf(stderr, "master socket can read\n");
#endif
			} else if ( evs[i].events & EPOLLIN) {
				/* 读事件 */
				client = http_client_get(evs[i].data.fd);
				ret = request_read(client, evs[i].data.fd);
			} else if ( evs[i].events & EPOLLOUT ) {
				/* 写事件 */
				client = http_client_get(evs[i].data.fd);
				ret = response_write(client, evs[i].data.fd);
			} else {
				/* ERROR */
				if ( evs[i].data.fd == sv ) {
					perror("sv error:");
					exit(-1);
				} else {
					fprintf(stderr, "error happened on socket: %d\n",
							evs[i].data.fd);
					client = http_client_get(evs[i].data.fd);
					ret = -1;
				}
			}

			switch ( ret ) {
				case HTTP_SOCKFD_IN:
					epoll_set_in(epfd, evs[i].data.fd, &ev);
					break;
				case HTTP_SOCKFD_OUT:
					epoll_set_out(epfd, evs[i].data.fd, &ev);
					break;
				case HTTP_SOCKFD_DEL:
				default :
					if ( client )
						http_client_put(client);
					epoll_skfd_del(epfd, evs[i].data.fd, &ev);
					break;
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
	cpu_set_t mask;
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
#ifdef __HTTP_ACCEPT_LOCK__ 
			/* 通过锁的方式互斥接收请求*/
			closefd = closefd; // make gcc happy.
#else
			close(closefd);
#endif
			for (j = i-1; j >= 0; j--) {
				close(worker_sv[j][1]);
			}

#if 1
			/* set worker CPU affinity */
			CPU_ZERO(&mask);
			CPU_SET(i, &mask);
			if ( sched_setaffinity(0, sizeof(mask), &mask) == -1) {
				fprintf(stderr,"Warning: Could not set CPU affinity!\n");
			}
#endif

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
