#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

struct skynet_config {
	int thread;     //工作线程数量
	int harbor;    //当前节点 id
	const char * daemon; // 后台运行存储的进程文件
	const char * module_path;//c 模块搜索路径
	const char * bootstrap; //启动文件
	const char * logger; //日志文件
};

#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4

void skynet_start(struct skynet_config * config);

#endif
