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

int main(int argc, char ** argv)
{
	int listenfd, sockfd;
	struct sockaddr_in serv_addr;
	struct sockaddr client_addr;
	int addr_len;
	int worker;
	int num_cpu;
	int i, ret = -1;
	
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

	num_cpu = sysconf(_SC_NPROCESSORS_CONF);

	worker = (num_cpu == -1 ? 4 : num_cpu);
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

	return 0;
}
