#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
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

static int send_to_worker(int sfd, int sockfd)
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

static int recv_from_master(int rfd)
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

/* worker 进程的工作函数，一直工作不退出 */
static void worker_process(int sv)
{
	int sockfd, epfd, nfds;
	struct epoll_event ev, evs[20];
	int i, ret;

	if ( sv < 0 ) {
		printf("worker_process parameter error!\n");
		return;
	}

	epfd = epoll_create(256);
	if ( epfd < 0 ) {
		perror("epoll_create error:");
		return;
	}
	
	ev.data.fd = sv;
	ev.events = EPOLLIN|EPOLLET;
	while ( (ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv, &ev) ) < 0 )
		if ( errno == EINTR )
			continue;
		else {
			perror("epoll_ctl error:");
			return;
		}
	while ( 1 ) {
		while ( ( nfds = epoll_wait(epfd, evs, 20, 500) ) < 0 ) {
			if ( errno == EINTR )
				continue;
			else {
				/* 此处如何容错??? */
				perror("epoll_wait error:");
			}
		}

		for (i = 0; i < nfds; i++) {
			if( evs[i].data.fd == sv ) {
				if ( (sockfd = recv_from_master(ev)) < 0 )
					continue;
				while ( ( ret = epoll_ctl(sockfd, EPOLL_CTL_ADD, sv, &ev) ) < 0 )
					if ( errno == EINTR )
						continue;
					else 
						perror("epoll_ctl error:");
			} else if ( evs[i].events & EPOLLIN) {
				request_process(evs[i].data.fd);
			} /* else if ( evs[i].events & EPOLLOUT ) {
				 TODO write data back to socket 
			} */
		}

		/* TODO: 从epfd中删除sockfd的动作没做完 */
	}
}

int main(int argc, char ** argv)
{
	int listenfd, sockfd;
	struct sockaddr_in serv_addr;
	struct sockaddr client_addr;
	int addr_len;
	int worker=1;
	pid_t pid;
	int	sv[2];
	int ret = -1;
	
	signal(SIGUSR1, mem_dump);
	signal(SIGCHLD, SIG_IGN);

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

	ret = socketpair(AF_LOCAL, SOCK_STREAM, 0, sv);
	if ( -1 == ret ) {
		perror("socketpair error: ");
		return ret;
	}

	/* TODO: 取得CPU的核数 */
	worker = 4;
	do { 
		pid = fork();
		if ( pid == 0 ) {
			close(listenfd);
			close(sv[1]);
			/* 永远不返回 */
			worker_process(sv[0]);
		} else if ( pid < 0 ) {
			perror("fork error: ");
			return ret;
		}
	} while ( worker-- );

	close(sv[0]);

	do {
		memset(&client_addr, '\0', addr_len);
		sockfd = accept(listenfd, (struct sockaddr *)&client_addr, &addr_len);
		if ( -1 == sockfd  && errno == EINTR ) 
			continue;
		else if ( -1 == sockfd ) {
			perror("accept error: ");
			return ret;
		}

		ret = send_to_worker(sv[1], sockfd);
		if ( -1 == ret )
			printf("send socket fd to worker error!\n");

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
