#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);

struct skynet_module {
	const char * name;  //库的名字
	void * module;   // 动态库的 打开句柄
	skynet_dl_create create;  // 库对外暴露的 create 接口
	skynet_dl_init init;     // 库对外暴露的 init 接口
	skynet_dl_release release;// 库对外暴露的 release 接口
};

void skynet_module_insert(struct skynet_module *mod); // 主动插入 module对象
struct skynet_module * skynet_module_query(const char * name); // 查询module对象，不在管理表中则尝试查找打开

void * skynet_module_instance_create(struct skynet_module *);
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);

void skynet_module_init(const char *path); //初始化库管理表结构

#endif
