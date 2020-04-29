#ifndef skynet_databuffer_h
#define skynet_databuffer_h

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MESSAGEPOOL 1023

struct message {
	char * buffer;  //存储的数据buffer
	int size;       //数据buffer的大小
	struct message * next; // 链表结构指向下个message
};

struct databuffer {
	int header;  //存储网络通信包的包头大小，默认为零，知道主动使用  databuffer_readheader 读取时才会从存储的数据中获取指定长度包头buffer中对应的包头size
	int offset; //当前读取message 的偏移位置   
	int size;  //整个 buffer 存储的数据size
	struct message * head;  //message 链表的第一个元素，方便读取
	struct message * tail;  //message 链表的最后一个元素，方便插入
};

struct messagepool_list {
	struct messagepool_list *next;  // 链表结构指向下个messagepool
	struct message pool[MESSAGEPOOL]; //当前pool可用的message
};

struct messagepool {
	struct messagepool_list * pool; //当前池中可用的pool
	struct message * freelist;  //当前池中空闲可用的message
};

// use memset init struct 

static void  // 释放 message pool
messagepool_free(struct messagepool *pool) {
	struct messagepool_list *p = pool->pool;
	while(p) {
		struct messagepool_list *tmp = p;
		p=p->next;
		skynet_free(tmp);
	}
	pool->pool = NULL;
	pool->freelist = NULL;
}

static inline void //将 databuffer 中的 head mesage 释放，策略是存入 messagepool 的释放池中
_return_message(struct databuffer *db, struct messagepool *mp) {
	struct message *m = db->head;
	if (m->next == NULL) {
		assert(db->tail == m);
		db->head = db->tail = NULL;
	} else {
		db->head = m->next;
	}
	skynet_free(m->buffer);
	m->buffer = NULL;
	m->size = 0;
	m->next = mp->freelist;
	mp->freelist = m;
}

static void //将 databuffer 中的数据读取 sz 大小数据到  buffer 中
databuffer_read(struct databuffer *db, struct messagepool *mp, void * buffer, int sz) {
	assert(db->size >= sz);
	db->size -= sz;
	for (;;) {
		struct message *current = db->head;
		int bsz = current->size - db->offset;
		if (bsz > sz) {
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset += sz;
			return;
		}
		if (bsz == sz) {
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset = 0;
			_return_message(db, mp);
			return;
		} else {
			memcpy(buffer, current->buffer + db->offset, bsz);
			_return_message(db, mp);
			db->offset = 0;
			buffer+=bsz;
			sz-=bsz;
		}
	}
}

static void //想databuffer 中插入 sz 大小的buffer。 策略为优先利用messagepool 中的空闲message 如果没有则新建
databuffer_push(struct databuffer *db, struct messagepool *mp, void *data, int sz) {
	struct message * m;
	if (mp->freelist) {
		m = mp->freelist;
		mp->freelist = m->next;
	} else {
		struct messagepool_list * mpl = skynet_malloc(sizeof(*mpl));
		struct message * temp = mpl->pool;
		int i;
		for (i=1;i<MESSAGEPOOL;i++) {
			temp[i].buffer = NULL;
			temp[i].size = 0;
			temp[i].next = &temp[i+1];
		}
		temp[MESSAGEPOOL-1].next = NULL;
		mpl->next = mp->pool;
		mp->pool = mpl;
		m = &temp[0];
		mp->freelist = &temp[1];
	}
	m->buffer = data;
	m->size = sz;
	m->next = NULL;
	db->size += sz;
	if (db->head == NULL) {
		assert(db->tail == NULL);
		db->head = db->tail = m;
	} else {
		db->tail->next = m;
		db->tail = m;
	}
}

static int  //获取当前 databuffer 存储的数据包的 包头大小
databuffer_readheader(struct databuffer *db, struct messagepool *mp, int header_size) {
	if (db->header == 0) {
		// parser header (2 or 4)
		if (db->size < header_size) {
			return -1;
		}
		uint8_t plen[4];
		databuffer_read(db,mp,(char *)plen,header_size);
		// big-endian
		if (header_size == 2) {
			db->header = plen[0] << 8 | plen[1];
		} else {
			db->header = plen[0] << 24 | plen[1] << 16 | plen[2] << 8 | plen[3];
		}
	}
	if (db->size < db->header)
		return -1;
	return db->header;
}

static inline void //重置包头大小，可能是databuffer 中一个完成的包读取完了需要重置
databuffer_reset(struct databuffer *db) {
	db->header = 0;
}

static void //清理 databuffer中的message，并进结构体数据清零，databuffer对应的内存还是在的
databuffer_clear(struct databuffer *db, struct messagepool *mp) {
	while (db->head) {
		_return_message(db,mp);
	}
	memset(db, 0, sizeof(*db));
}

#endif
