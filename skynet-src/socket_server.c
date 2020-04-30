#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16
#define MAX_EVENT 64			  // 同时读取的网络事件最大值
#define MIN_READ_BUFFER 64		  // socket 读取数据buffer的最小size
#define SOCKET_TYPE_INVALID 0     // 无效的 socket
#define SOCKET_TYPE_RESERVE 1     // 新分配的 socket 还等待指定角色状态的
#define SOCKET_TYPE_PLISTEN 2     // 当前已经做好监听准备工作也增加好了事件监听工作，等待 start 操作就可以正式工作了，是 SOCKET_TYPE_LISTEN 的前置状态
#define SOCKET_TYPE_LISTEN 3		// 进入了正式的监听接收客户端连接的工作了
#define SOCKET_TYPE_CONNECTING 4     // 还在连接中的状态，发起了 连接事件但是还没建立连接成功
#define SOCKET_TYPE_CONNECTED 5      //socket处于connect连接成功的状态
#define SOCKET_TYPE_HALFCLOSE 6    // socket 处于半关闭状态， 代表的是我们已经发起了关闭操作，但是由于socket还有未发送完的数据，则先标记关闭状态等发送完了再关闭
#define SOCKET_TYPE_PACCEPT 7      // accpet 接收到的连接状态，但是还没有正式进入数据通信状态，还等待业务层确认是否认证通过,再转变成 SOCKET_TYPE_CONNECTED
#define SOCKET_TYPE_BIND 8

#define MAX_SOCKET (1<<MAX_SOCKET_P)  // 网络模块最多管理的 socket 数量

#define PRIORITY_HIGH 0    // socket 数据发送优先级定义  此值代表高优先级
#define PRIORITY_LOW 1     // socket 数据发送优先级定义  此值代表低优先级

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)

struct write_buffer { // 发送消息的 buffer 结构，这里能确定的是 一个 writebuffer 里的buffer是一个完整的消息包
	struct write_buffer * next;
	char *ptr;  // 读buffer的指针，代表当前读取位置
	int sz; // buffer 的 大小
	void *buffer; // 发送的buffer
};

struct wb_list {
	struct write_buffer * head;  //发送消息的列表头  方便用于读取发送数据
	struct write_buffer * tail; //发送消息队列的尾部，方便与插入新的发送数据
};

struct socket {
	int fd;   // socket 句柄
	int id;   // 在 socket server 管理器中的id
	int type; // 当前 socket 的状态
	int size;  // socket 读数据时 需要的起始 buffer size，默认为 MIN_READ_BUFFER = 64, 如果大小不够时会按 2 的指数增加
	int64_t wb_size;  //当前需要发送的数据size
	uintptr_t opaque;  // 这里 是一个透传字段，存储的是发起 网络操作的 服务handle，也就是 这个 socket 所属 服务 handle
	struct wb_list high;  // socket 发送数据队列，高优先急的发送数据
	struct wb_list low;   // socket 发送数据队列，低优先级的发送数据， 只有当高优先急发送完才会处理低优先级的
};

struct socket_server {
	int recvctrl_fd;  // 接受操作命令的句柄  是pip通道的句柄
	int sendctrl_fd;  // 发送命令的句柄      是pip通道的句柄
	int checkctrl;    // 标记当前是否可以处理操作命令，此标记主要用来控制优先处理我们的网络通信数据，再操作内部的处理命令
	poll_fd event_fd; // 网络时间驱动核心
	int alloc_id;	  // 当前分配的 socket id 计数器
	int event_n;      // 当前接收到的网络事件数量
	int event_index;  // 当前接受到的网络事件当前处理中的时间 索引值
	struct event ev[MAX_EVENT];   // 网络事件 接收数组 最大同时读取 64 个事件
	struct socket slot[MAX_SOCKET]; //当前网络层socket 连接的数组，最大 0x10000 =  65536 个连接
	char buffer[MAX_INFO];
	fd_set rfds;  // 用于 select 查询 pip recvctrl_fd 是否可读用的 
};

struct request_open { //用于请求 connect 的
	int id;
	int port;
	uintptr_t opaque;  // 这里 是一个透传字段，存储的是发起 网络操作的 服务handle，也就是 这个 socket 所属 服务 handle
	char host[1];
};

struct request_send { // 用于发送 数据用
	int id;
	int sz;
	char * buffer;
};

struct request_close {  // 用于关闭 socket 用
	int id;
	uintptr_t opaque; // 这里 是一个透传字段，存储的是发起 网络操作的 服务handle，也就是 这个 socket 所属 服务 handle
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque; // 这里 是一个透传字段，存储的是发起 网络操作的 服务handle，也就是 这个 socket 所属 服务 handle
	char host[1];
};

struct request_bind {  // 绑定
	int id;
	int fd;
	uintptr_t opaque; // 这里 是一个透传字段，存储的是发起 网络操作的 服务handle，也就是 这个 socket 所属 服务 handle
};

struct request_start {
	int id;
	uintptr_t opaque; // 这里 是一个透传字段，存储的是发起 网络操作的 服务handle，也就是 这个 socket 所属 服务 handle
};

struct request_setopt { // 请求设置 socket 属性
	int id; 
	int what;  // 要设置的属性
	int value; // 要设置的值
};

struct request_package {
	uint8_t header[8];	// 6 bytes dummy
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
	} u;
	uint8_t dummy[256];
};

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

#define MALLOC skynet_malloc
#define FREE skynet_free


// 设置 socket 举报的 keppalive 属性，作用为当socket连接对方非合法关闭链接时，能在一定时间后检测到这个状态，然后关闭本地的连接
// 默认的间隔时间是 2小时，可以设置网络属性来控制这个时间
// 参考文档  https://blog.csdn.net/chenlycly/article/details/51790941
static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

// 分配一个新的struct socket 并返回其id值
static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = __sync_add_and_fetch(&(ss->alloc_id), 1);
		if (id < 0) {
			id = __sync_and_and_fetch(&(ss->alloc_id), 0x7fffffff);
		}
		struct socket *s = &ss->slot[HASH_ID(id)];
		if (s->type == SOCKET_TYPE_INVALID) {
			if (__sync_bool_compare_and_swap(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				s->id = id;
				s->fd = -1;
				return id;
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}

static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}

// 创建网络层的 核心对象 socket server ，包含一个 epoll 的核心句柄，一个读写pip 用于读写命令控制网络操作
struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];
	poll_fd efd = sp_create();
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}
	if (pipe(fd)) { // 创建一个 读写管道  fd[0] -> r fd[1] -> w  https://blog.csdn.net/qq_42914528/article/details/82023408
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	struct socket_server *ss = MALLOC(sizeof(*ss));
	ss->event_fd = efd;
	ss->recvctrl_fd = fd[0];
	ss->sendctrl_fd = fd[1];
	ss->checkctrl = 1;

	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
	}
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	FD_ZERO(&ss->rfds);
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

static void
free_wb_list(struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		FREE(tmp->buffer);
		FREE(tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);
	free_wb_list(&s->high);
	free_wb_list(&s->low);
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {
		sp_del(ss->event_fd, s->fd);
	}
	if (s->type != SOCKET_TYPE_BIND) {
		close(s->fd);
	}
	s->type = SOCKET_TYPE_INVALID;
}

void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s , &dummy);
		}
	}
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);
	sp_release(ss->event_fd);
	FREE(ss);
}

static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

// 初始化一个新分配 socket
// 主要工作是 将 fd 套接字 加入我们的 管理器，然后根据 add 参数决定是否监听可读事件
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (add) {
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;
	s->size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	check_wb_list(&s->high);
	check_wb_list(&s->low);
	return s;
}

// return -1 when connecting
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	struct socket *ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port, "%d", request->port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	status = getaddrinfo( request->host, port, &ai_hints, &ai_list ); //获取要连接 host port 地址族列表
	if ( status != 0 ) {
		goto _failed;
	}
	int sock= -1;
	//尝试连接返回的 地址家族，直到返回一个可以connect 成功的
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}
		socket_keepalive(sock);
		sp_nonblocking(sock);
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;
			continue;
		}
		sp_nonblocking(sock);
		break;
	}

	if (sock < 0) {
		goto _failed;
	}

	ns = new_fd(ss, id, sock, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		goto _failed;
	}

	if(status == 0) {
		ns->type = SOCKET_TYPE_CONNECTED;
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {
		ns->type = SOCKET_TYPE_CONNECTING;
		sp_write(ss->event_fd, ns->fd, ns, true); //增加 写状态的监听
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

//将 wb_list 中的数据发送出去
static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				switch(errno) {
				case EINTR:  //写的过程中中断了，这里我只需要重新再来过就行
					continue;
				case EAGAIN: // 当前socket 的发送队列满了，那么我们就暂时不要发送数据了直接退出发送任务
					return -1;
				}
				force_close(ss,s, result);
				return SOCKET_CLOSE;
			}
			s->wb_size -= sz;
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		list->head = tmp->next;
		FREE(tmp->buffer);
		FREE(tmp);
	}
	list->tail = NULL;

	return -1;
}

// 判断 wb_list 是否未完成 
static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	return (void *)wb->ptr != wb->buffer;
}

// 当socket的发送队列为空是，将低优先级的发送数据升级到高优先级队列发送
static void
raise_uncomplete(struct socket * s) {
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	struct wb_list *high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	assert(!list_uncomplete(&s->low));  //断言 低优先级的发送队列为空
	// step 1
	if (send_list(ss,s,&s->high,result) == SOCKET_CLOSE) { // 将高优先级发送数据全部发送，除非塞满发送队列否则会清空队列
		return SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {  // 高优先急数据清空了
		// step 2
		if (s->low.head != NULL) {  //低优先级还有数据
			if (send_list(ss,s,&s->low,result) == SOCKET_CLOSE) { //尝试将低优先级队列数据全部发送了，除非塞满发送队列
				return SOCKET_CLOSE;
			}
			// step 3
			if (list_uncomplete(&s->low)) {  //如果低优先级 未发送完毕，则将升级一个低优先级消息数据成高优先急
				raise_uncomplete(s);
			}
		} else {
			// step 4
			sp_write(ss->event_fd, s->fd, s, false); //如果数据发送完毕了，则清理掉可写状态的监听

			if (s->type == SOCKET_TYPE_HALFCLOSE) {  // 如果当前socket 本地已经发起了close 操作，数据发送完了我们就断开连接
				force_close(ss, s, result);
				return SOCKET_CLOSE;
			}
		}
	}

	return -1;
}


//在 发送数据列表中插入发送数据
static int
append_sendbuffer_(struct wb_list *s, struct request_send * request, int n) {
	struct write_buffer * buf = MALLOC(sizeof(*buf));
	buf->ptr = request->buffer+n;
	buf->sz = request->sz - n;
	buf->buffer = request->buffer;
	buf->next = NULL;
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf->sz;
}

// 往发送数据高优先急队列增加数据
static inline void
append_sendbuffer(struct socket *s, struct request_send * request, int n) {
	s->wb_size += append_sendbuffer_(&s->high, request, n);
}

// 往发送数据低优先急队列增加数据
static inline void
append_sendbuffer_low(struct socket *s, struct request_send * request) {
	s->wb_size += append_sendbuffer_(&s->low, request, 0);
}

// 判断 socket 的发送队列为空
static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.

	发送消息，流程
	1、如果socket本身没有未发送完成的数据则直接将buffer 发送了
	2、如果 直接发送buffer 不完则将buffer存入发送队列，并增加监听可写状态
 */
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		FREE(request->buffer);
		return -1;
	}
	assert(s->type != SOCKET_TYPE_PLISTEN && s->type != SOCKET_TYPE_LISTEN);
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
		int n = write(s->fd, request->buffer, request->sz);
		if (n<0) {
			switch(errno) {
			case EINTR:
			case EAGAIN:
				n = 0;
				break;
			default:
				fprintf(stderr, "socket-server: write to %d (fd=%d) error.",id,s->fd);
				force_close(ss,s,result);
				return SOCKET_CLOSE;
			}
		}
		if (n == request->sz) {
			FREE(request->buffer);
			return -1;
		}
		append_sendbuffer(s, request, n);	// add to high priority list, even priority == PRIORITY_LOW
		sp_write(ss->event_fd, s->fd, s, true);
	} else {
		if (priority == PRIORITY_LOW) {
			append_sendbuffer_low(s, request);
		} else {
			append_sendbuffer(s, request, 0);
		}
	}
	return -1;
}

// 这里的不是 socket 那个监听端口等待连接的操作，因为到这个阶段的时候的 套接字已经处理了bind 和 listen 操作，做好了监听工作
// 这里只是简单的将 socket 和我们的管理器关联起来
static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;
	int listen_fd = request->fd;
	struct socket *s = new_fd(ss, id, listen_fd, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;
	return -1;
_failed:
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERROR;
}


// 尝试关闭 socket，如果socket 数据发送完了直接关闭，如果未发送完成则需要等数据真正发送完了才会真实关闭
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}
	if (!send_buffer_empty(s)) { //如果socket还存着未发送完的数据则先将数据发送完
		int type = send_buffer(ss,s,result);
		if (type != -1) // 消息发送失败，直接返回
			return type;
	}
	if (send_buffer_empty(s)) {  //已经发送完毕，则直接关闭
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	s->type = SOCKET_TYPE_HALFCLOSE; // 还有数据未发送完毕，则先标记成半关闭状态，等数据发送完了我们再彻底关闭

	return -1;
}


// 这里的绑定操作不是 socket 那个绑定端口的操作
// 这里是代表将 socket 绑定到我们的网络管理器中，并增加读事件的监听,并且将socket设置成非阻塞的状态
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	struct socket *s = new_fd(ss, id, request->fd, request->opaque, true);
	if (s == NULL) {
		result->data = NULL;
		return SOCKET_ERROR;
	}
	sp_nonblocking(request->fd);
	s->type = SOCKET_TYPE_BIND;
	result->data = "binding";
	return SOCKET_OPEN;
}

static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return SOCKET_ERROR;
	}
	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {
		if (sp_add(ss->event_fd, s->fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return SOCKET_ERROR;
		}
		// 如果socket 原来是新 accpet 的则转变成 连接成功状态，如果原先是 新建立的监听socket则转变成 监听连接成功状态
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	} else if (s->type == SOCKET_TYPE_CONNECTED) { // 本来就是连接成功的socket 则直接啥也不用做等待数据传输就行了
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	return -1;
}


// 设置socket的 属性
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return;
	}
	int v = request->value;
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}


// 阻塞模式读取 管道中的数据
static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {
		int n = read(pipefd, buffer, sz);
		if (n<0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "socket-server : read pipe error %s.",strerror(errno));
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);
		return;
	}
}

// 判断管道中是否有 命令可读
static int
has_cmd(struct socket_server *ss) {
	struct timeval tv = {0,0};
	int retval;

	FD_SET(ss->recvctrl_fd, &ss->rfds);

	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);
	if (retval == 1) {
		return 1;
	}
	return 0;
}


// 阻塞式读取管道中的命令，并处理
// return type
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];
	uint8_t header[2];
	block_readpipe(fd, header, sizeof(header));
	int type = header[0];
	int len = header[1];
	block_readpipe(fd, buffer, len);
	// ctrl command only exist in local fd, so don't worry about endian.
	switch (type) {
	case 'S':
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':
		return open_socket(ss, (struct request_open *)buffer, result);
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_HIGH);
	case 'P':
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_LOW);
	case 'T':
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// 从 socket中尝试读取数据，如果读取数据失败则返回 -1  如果读取到了则返回 SOCKET_DATA = 0
// return -1 (ignore) when error
static int
forward_message(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	int sz = s->size;
	char * buffer = MALLOC(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			force_close(ss, s, result);
			return SOCKET_ERROR;
		}
		return -1;
	}
	if (n==0) {
		FREE(buffer);
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}
	// 发起了关闭操作的socket 不再接收新数据
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	if (n == sz) { //读满buffer则增加 下次的buffer size
		s->size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) { //当可读数据减少了则减小 下次读取的buffer size
		s->size /= 2;
	}

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return SOCKET_DATA;
}

static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);
	// 读取socket 的错误码，如果读取失败或者socket存在错误则关闭socket
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error) {  
		force_close(ss,s, result);
		return SOCKET_ERROR;
	} else {
		s->type = SOCKET_TYPE_CONNECTED; // 标记成连接成功状态
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		if (send_buffer_empty(s)) { //如果存在要发送数据则监听可写状态
			sp_write(ss->event_fd, s->fd, s, false);
		}

		// 读取连接对方的 地址
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// return 0 when failed
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0) {
		return 0;
	}
	int id = reserve_id(ss);// 请求分配一个管理id
	if (id < 0) { // 如果网络层 管理的连接满了则拒绝新连接
		close(client_fd);
		return 0;
	}
	socket_keepalive(client_fd);
	sp_nonblocking(client_fd);
	struct socket *ns = new_fd(ss, id, client_fd, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}
	ns->type = SOCKET_TYPE_PACCEPT;
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
		result->data = ss->buffer;
	}

	return 1;
}

static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {
	if (type == SOCKET_CLOSE || type == SOCKET_ERROR) {
		int id = result->id;
		int i;
		for (i=ss->event_index; i<ss->event_n; i++) {
			struct event *e = &ss->ev[i];
			struct socket *s = e->s;
			if (s) {
				if (s->type == SOCKET_TYPE_INVALID && s->id == id) {
					e->s = NULL;
				}
			}
		}
	}
}

// return type
//  函数有两个工作
// 1、 在没有事件未处理完的情况下回读取业务发起的网络 操作命令
// 2、询问内核是否有新的可处理网络事件操作，如果有则处理事件
//  返回值为 操作网络事件的 结果
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {
		if (ss->checkctrl) {
			if (has_cmd(ss)) {
				int type = ctrl_cmd(ss, result);
				if (type != -1) { // 读取操作命令异常
					clear_closed_event(ss, result, type);
					return type;
				} else
					continue;
			} else {
				ss->checkctrl = 0;
			}
		}
		//当前获取到的时间都处理完了则等待新的时间，并标记可以处理其他的网络操作事件了
		if (ss->event_index == ss->event_n) {
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			ss->checkctrl = 1;
			if (more) {
				*more = 0;
			}
			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}
		struct event *e = &ss->ev[ss->event_index++];
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			continue;
		}
		switch (s->type) {
		case SOCKET_TYPE_CONNECTING: //连接中的 socket 收到时间代表连接状态有变化，要么连接成功，要么连接失败了
			return report_connect(ss, s, result);
		case SOCKET_TYPE_LISTEN: // 监听状态的socket 有状态则代表 有新连接了
			if (report_accept(ss, s, result)) {
				return SOCKET_ACCEPT;
			} 
			break;
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:
			if (e->write) {
				int type = send_buffer(ss, s, result);
				if (type == -1) // 发送正常
					break;
				clear_closed_event(ss, result, type); //发送异常了 清理掉
				return type;
			}
			if (e->read) {
				int type = forward_message(ss, s, result);
				if (type == -1) // 接收正常
					break;
				clear_closed_event(ss, result, type); // 接收数据异常，清理掉
				return type;
			}
			break;
		}
	}
}

// 向网络模块发送操作请求
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	for (;;) {
		int n = write(ss->sendctrl_fd, &request->header[6], len+2);
		if (n<0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

//
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) > 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return 0;
	}
	int id = reserve_id(ss);
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}

// return -1 when error
int64_t 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}

void 
socket_server_send_lowpriority(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'P', sizeof(request.u.send));
}

void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

static int
do_listen(const char * host, int port, int backlog) {
	// only support ipv4
	// todo: support ipv6 by getaddrinfo
	uint32_t addr = INADDR_ANY;
	if (host[0]) {
		addr=inet_addr(host);
	}
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		return -1;
	}
	int reuse = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}

	struct sockaddr_in my_addr;
	memset(&my_addr, 0, sizeof(struct sockaddr_in));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	my_addr.sin_addr.s_addr = addr;
	if (bind(listen_fd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1) {
		goto _failed;
	}
	if (listen(listen_fd, backlog) == -1) {
		goto _failed;
	}
	return listen_fd;
_failed:
	close(listen_fd);
	return -1;
}

int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	struct request_package request;
	int id = reserve_id(ss);
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;
	send_request(ss, &request, 'L', sizeof(request.u.listen));
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.start));
}


// 设置socket 属性，关闭  Nagle ，大概意思是 socket管道上的数据包都会立即发送，不会走优化算法进行一些小包合并的处理
// 这样做的好处是 减少了消息通信的延迟新，但是增加了通信频率的代价，不过在目前网络高速的情况这些都是可以接受的
void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}
