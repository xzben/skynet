#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);

struct skynet_module {
	const char * name;  //�������
	void * module;   // ��̬��� �򿪾��
	skynet_dl_create create;  // ����Ⱪ¶�� create �ӿ�
	skynet_dl_init init;     // ����Ⱪ¶�� init �ӿ�
	skynet_dl_release release;// ����Ⱪ¶�� release �ӿ�
};

void skynet_module_insert(struct skynet_module *mod); // �������� module����
struct skynet_module * skynet_module_query(const char * name); // ��ѯmodule���󣬲��ڹ���������Բ��Ҵ�

void * skynet_module_instance_create(struct skynet_module *);
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);

void skynet_module_init(const char *path); //��ʼ��������ṹ

#endif
