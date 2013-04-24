#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include "worker.h"

#ifdef __HTTP_ACCEPT_LOCK__
#include <sys/epoll.h>
#include <sys/mman.h>
#endif

int main(int argc, char ** argv)
{
	struct sockaddr_in serv_addr;
#ifndef __HTTP_ACCEPT_LOCK__ 
	int listenfd, sockfd;
	struct sockaddr client_addr;
#else
	int	master_epfd, nfd;
	struct epoll_event ev, evs[20];
	int j;
#endif
	int addr_len;
	int worker;
	int num_cpu;
	int i, ret = -1;
	
	signal(SIGCHLD, SIG_IGN);
	/* TODO SIGPIPE */
	// signal(SIGPIPE, SIG_IGN);

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

#ifdef __HTTP_ACCEPT_LOCK__ 
	/* 初始化accept锁 */
	while ( 1 ) {
		accept_lock = (sem_t *)mmap(NULL, sizeof(sem_t),
				PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
		if( accept_lock == MAP_FAILED && errno == EINTR )
			continue;
		else if ( accept_lock == MAP_FAILED ) {
			perror("mmap failed: ");
			exit(-1);
		}
		break;
	}
	if ( sem_init(accept_lock, 1, 1) == -1 ) {
		perror("Cannot init accept lock: ");
		exit(-1);
	}
#endif

	num_cpu = sysconf(_SC_NPROCESSORS_CONF);

/*#define HTTP_DEBUG */
#ifndef HTTP_DEBUG
	worker = (num_cpu == -1 ? 4 : num_cpu);
#else
	worker = 1;
#endif
	
	int (*worker_sv)[2];

	worker_sv = (int (*)[2])calloc(worker, sizeof(int)*2);
	if ( NULL == worker_sv ) {
		fprintf(stderr, "Memory alloc failed!\n");
		return ret;
	}

	ret = create_worker(worker, worker_sv, listenfd);
	if ( -1 == ret ) {
		close(listenfd);
		_exit(-1);
	}
	
	/* 改成通过锁的方式，各个子进程分别接受请求，
	 * UNIX domain socket并不删除，继续保留将来使用。
	 */
#ifdef __HTTP_ACCEPT_LOCK__ 
	while ( 1 ) {
		master_epfd = epoll_create(10);
		if ( master_epfd < 0 && errno == EINTR )
			continue;
		else if ( master_epfd < 0 ) {
			perror("epoll_create error: ");
			exit(-1);
		}
		break;
	}

	for ( i = 0; i < worker; i++ ) {
		ev.data.fd = worker_sv[i][1];
		ev.events = EPOLLIN;
		while ( 1 ) {
			ret = epoll_ctl(master_epfd, EPOLL_CTL_ADD, worker_sv[i][1], &ev);
			if ( ret < 0 && errno == EINTR )
				continue;
			else if ( ret < 0 ) {
				perror("epoll_ctl error: ");
				exit(-1);
			}
			break;
		}
	}

	while ( 1 ) {
		while ( 1 ) {
			nfd = epoll_wait(master_epfd, evs, 20, -1);
			if ( nfd < 0 && errno == EINTR )
				continue;
			else if ( nfd < 0 ) {
				perror("epoll_wait error: ");
				exit(-1);
			}
			break;
		}

		for (i = 0; i < nfd; i++) {
			for (j = 0; j < worker; j++) {
				if ( evs[i].data.fd == worker_sv[j][i])
					fprintf(stderr, "worker: #%d can read\n", j);
			}
		}
		/* TODO:目前只做到master进程一直循环不推出 */
	}

#else
	i = 0;
	do {
		memset(&client_addr, '\0', addr_len);
		sockfd = accept(listenfd, (struct sockaddr *)&client_addr, &addr_len);
		if ( -1 == sockfd  && errno == EINTR ) 
			continue;
		else if ( -1 == sockfd ) {
			perror("accept error: ");
			return ret;
		}

		/* 轮询 */
		if ( i >= worker ) 
			i = 0;
		ret = send_to_worker(worker_sv[i][1], sockfd);
		if ( -1 == ret )
			perror("send to worker:");

		while ( 1 ) {
			ret = close(sockfd);
			if ( -1 == ret && errno == EINTR )
				continue;
			else if ( -1 == ret ) { 
				perror("close error: ");
			}
			break;
		}
	} while ( ++i );
#endif 

	return 0;
}
