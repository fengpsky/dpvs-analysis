/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/queue.h>
#include <sys/syscall.h>

#include <rte_debug.h>
#include <rte_atomic.h>
#include <rte_launch.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_per_lcore.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_eal_trace.h>

#include "eal_private.h"
#include "eal_thread.h"

/*
 * Send a message to a worker lcore identified by worker_id to call a
 * function f with argument arg. Once the execution is done, the
 * remote lcore switch in FINISHED state.
 */
int
rte_eal_remote_launch(int (*f)(void *), void *arg, unsigned int worker_id)
{
	int n;
	char c = 0;
	int m2w = lcore_config[worker_id].pipe_main2worker[1];
	int w2m = lcore_config[worker_id].pipe_worker2main[0];
	int rc = -EBUSY;

	if (lcore_config[worker_id].state != WAIT)
		goto finish;

	/* 注册回调函数和参数 */
	lcore_config[worker_id].f = f;
	lcore_config[worker_id].arg = arg;

	/* send message 
	获取主线程向子线程通信所用管道，主线程发送数据的file descriptor(m2s)

    获取子线程向主线程通信所用管道，主线程读取数据的file descriptor(s2m)*/
	n = 0;
	while (n == 0 || (n < 0 && errno == EINTR))
		n = write(m2w, &c, 1);/* 发送消息给SLAVE lcore */
	if (n < 0)
		rte_panic("cannot write on configuration pipe\n");

	/* wait ack *//* 等待SLAVE lcore的确认 */
	do {
		n = read(w2m, &c, 1);
	} while (n < 0 && errno == EINTR);

	if (n <= 0)
		rte_panic("cannot read on configuration pipe\n");

	rc = 0;
finish:
	rte_eal_trace_thread_remote_launch(f, arg, worker_id, rc);
	return rc;
}

/* main loop of threads */
__rte_noreturn void *
eal_thread_loop(__rte_unused void *arg)
{
	char c;
	int n, ret;
	unsigned lcore_id;
	pthread_t thread_id;
	int m2w, w2m;
	char cpuset[RTE_CPU_AFFINITY_STR_LEN];

	thread_id = pthread_self();

	/* retrieve our lcore_id from the configuration structure */
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (thread_id == lcore_config[lcore_id].thread_id)
			break;
	}
	if (lcore_id == RTE_MAX_LCORE)
		rte_panic("cannot retrieve lcore id\n");

	m2w = lcore_config[lcore_id].pipe_main2worker[0];
	w2m = lcore_config[lcore_id].pipe_worker2main[1];

	__rte_thread_init(lcore_id, &lcore_config[lcore_id].cpuset);
	/* 绑定SLAVE lcore到logical CPU */
	ret = eal_thread_dump_current_affinity(cpuset, sizeof(cpuset));
	RTE_LOG(DEBUG, EAL, "lcore %u is ready (tid=%zx;cpuset=[%s%s])\n",
		lcore_id, (uintptr_t)thread_id, cpuset, ret == 0 ? "" : "...");

	rte_eal_trace_thread_lcore_ready(lcore_id, cpuset);

	/* read on our pipe to get commands */
	while (1) {
		void *fct_arg;

		/* wait command 等待读取主线程发送的命令 */
		do {
			n = read(m2w, &c, 1);
		} while (n < 0 && errno == EINTR);

		if (n <= 0)
			rte_panic("cannot read on configuration pipe\n");

		lcore_config[lcore_id].state = RUNNING;//设置线程状态为RUNNING

		/* send ack 向主线程发送ack*/
		n = 0;
		while (n == 0 || (n < 0 && errno == EINTR))
			n = write(w2m, &c, 1);
		if (n < 0)
			rte_panic("cannot write on configuration pipe\n");

		if (lcore_config[lcore_id].f == NULL)
			rte_panic("NULL function pointer\n");

		/* call the function and store the return value 
		读取当前lcore对应的structlcore_config结构中的lcore_function_t类型函数指针，及调用参数*/
		fct_arg = lcore_config[lcore_id].arg;
		/* 执行MASTER lcore通过rte_eal_remote_launch()注册的回调函数
		 * 注意：大部分DPDK应用的回调函数都是一个死循环，SLAVE lcore会阻塞在这里 */
		ret = lcore_config[lcore_id].f(fct_arg);//执行所指函数，并存储返回值
		lcore_config[lcore_id].ret = ret;
		rte_wmb();

		/* when a service core returns, it should go directly to WAIT
		 * state, because the application will not lcore_wait() for it.
		 */
		if (lcore_config[lcore_id].core_role == ROLE_SERVICE)
			lcore_config[lcore_id].state = WAIT;
		else/* 设置SLAVE lcore的状态为FINISHED */
			lcore_config[lcore_id].state = FINISHED;//设置线程状态为FINISHED
	}

	/* never reached */
	/* pthread_exit(NULL); */
	/* return NULL; */
}

/* require calling thread tid by gettid() */
int rte_sys_gettid(void)
{
	return (int)syscall(SYS_gettid);
}

int rte_thread_setname(pthread_t id, const char *name)
{
	int ret = ENOSYS;
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 12)
	char truncated[16];

	strlcpy(truncated, name, sizeof(truncated));
	ret = pthread_setname_np(id, truncated);
#endif
#endif
	RTE_SET_USED(id);
	RTE_SET_USED(name);
	return -ret;
}

int rte_thread_getname(pthread_t id, char *name, size_t len)
{
	int ret = ENOSYS;
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 12)
	ret = pthread_getname_np(id, name, len);
#endif
#endif
	RTE_SET_USED(id);
	RTE_SET_USED(name);
	RTE_SET_USED(len);
	return -ret;

}
