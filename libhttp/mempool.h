#ifndef __MEMPOOL_H_
#define __MEMPOOL_H_
#include	<stdlib.h>

/* 功能: 简单的实现内存池管理 */

struct list {
	void *pre;
	void *next;
	void *data;
};

struct pool_entry{
	struct list node;
	void * buff;
	int total_size, inuse_size;
	int refcount;
};

#define MEMPOOL_FREE_MAX  256

/* 新节点插入方向 */
#define MEMPOOL_LITTLE		1
#define MEMPOOL_BIG			2
int mem_pool_entry_get(struct pool_entry **pentry, int size);
void mem_pool_entry_put(struct pool_entry *pentry);

#define __MEMPOOL_DEBUG_
#ifdef __MEMPOOL_DEBUG_
#include <stdio.h>
void mem_pool_dump();
#else
#define mem_pool_dump() ()
#endif

#endif
