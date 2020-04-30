#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

// epoll 详细介绍参考  https://blog.csdn.net/yusiguyuan/article/details/15027821
//

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

static bool 
sp_invalid(int efd) {
	return efd == -1;
}


// 创建 epoll 句柄
static int
sp_create() {
	return epoll_create(1024);
}

// 释放 epoll 句柄
static void
sp_release(int efd) {
	close(efd);
}

// 向 epoll 注册 socket 关注 socket 的可读状态
static int 
sp_add(int efd, int sock, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN; // 代表关心的是 socket的可读状态，当socket可读是触发事件
	ev.data.ptr = ud;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

// 从 epoll 中删除注册的 socket 不再关心此socket的事件
static void 
sp_del(int efd, int sock) {
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}

//将已经监听的 socket 的关心事件，默认关心可读状态，可写状态当 enable 为 true 则也监视
static void 
sp_write(int efd, int sock, void *ud, bool enable) {
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}


// 查询 max 数量的socket 读写状态事件。返回当前有时间触发的socket
static int 
sp_wait(int efd, struct event *e, int max) {
	struct epoll_event ev[max];
	int n = epoll_wait(efd , ev, max, -1);
	int i;
	for (i=0;i<n;i++) {
		e[i].s = ev[i].data.ptr;
		unsigned flag = ev[i].events;
		e[i].write = (flag & EPOLLOUT) != 0;
		e[i].read = (flag & EPOLLIN) != 0;
	}

	return n;
}

// 将socket 设置成 非阻塞模式
static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
