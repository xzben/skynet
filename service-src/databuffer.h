#ifndef skynet_databuffer_h
#define skynet_databuffer_h

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MESSAGEPOOL 1023

struct message {
	char * buffer;  //�洢������buffer
	int size;       //����buffer�Ĵ�С
	struct message * next; // ����ṹָ���¸�message
};

struct databuffer {
	int header;  //�洢����ͨ�Ű��İ�ͷ��С��Ĭ��Ϊ�㣬֪������ʹ��  databuffer_readheader ��ȡʱ�Ż�Ӵ洢�������л�ȡָ�����Ȱ�ͷbuffer�ж�Ӧ�İ�ͷsize
	int offset; //��ǰ��ȡmessage ��ƫ��λ��   
	int size;  //���� buffer �洢������size
	struct message * head;  //message ����ĵ�һ��Ԫ�أ������ȡ
	struct message * tail;  //message ��������һ��Ԫ�أ��������
};

struct messagepool_list {
	struct messagepool_list *next;  // ����ṹָ���¸�messagepool
	struct message pool[MESSAGEPOOL]; //��ǰpool���õ�message
};

struct messagepool {
	struct messagepool_list * pool; //��ǰ���п��õ�pool
	struct message * freelist;  //��ǰ���п��п��õ�message
};

// use memset init struct 

static void  // �ͷ� message pool
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

static inline void //�� databuffer �е� head mesage �ͷţ������Ǵ��� messagepool ���ͷų���
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

static void //�� databuffer �е����ݶ�ȡ sz ��С���ݵ�  buffer ��
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

static void //��databuffer �в��� sz ��С��buffer�� ����Ϊ��������messagepool �еĿ���message ���û�����½�
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

static int  //��ȡ��ǰ databuffer �洢�����ݰ��� ��ͷ��С
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

static inline void //���ð�ͷ��С��������databuffer ��һ����ɵİ���ȡ������Ҫ����
databuffer_reset(struct databuffer *db) {
	db->header = 0;
}

static void //���� databuffer�е�message�������ṹ���������㣬databuffer��Ӧ���ڴ滹���ڵ�
databuffer_clear(struct databuffer *db, struct messagepool *mp) {
	while (db->head) {
		_return_message(db,mp);
	}
	memset(db, 0, sizeof(*db));
}

#endif
