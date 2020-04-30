#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

// epoll ��ϸ���ܲο�  https://blog.csdn.net/yusiguyuan/article/details/15027821
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


// ���� epoll ���
static int
sp_create() {
	return epoll_create(1024);
}

// �ͷ� epoll ���
static void
sp_release(int efd) {
	close(efd);
}

// �� epoll ע�� socket ��ע socket �Ŀɶ�״̬
static int 
sp_add(int efd, int sock, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN; // ������ĵ��� socket�Ŀɶ�״̬����socket�ɶ��Ǵ����¼�
	ev.data.ptr = ud;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

// �� epoll ��ɾ��ע��� socket ���ٹ��Ĵ�socket���¼�
static void 
sp_del(int efd, int sock) {
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}

//���Ѿ������� socket �Ĺ����¼���Ĭ�Ϲ��Ŀɶ�״̬����д״̬�� enable Ϊ true ��Ҳ����
static void 
sp_write(int efd, int sock, void *ud, bool enable) {
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}


// ��ѯ max ������socket ��д״̬�¼������ص�ǰ��ʱ�䴥����socket
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

// ��socket ���ó� ������ģʽ
static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
