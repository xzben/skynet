#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

struct skynet_config {
	int thread;     //�����߳�����
	int harbor;    //��ǰ�ڵ� id
	const char * daemon; // ��̨���д洢�Ľ����ļ�
	const char * module_path;//c ģ������·��
	const char * bootstrap; //�����ļ�
	const char * logger; //��־�ļ�
};

#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4

void skynet_start(struct skynet_config * config);

#endif
