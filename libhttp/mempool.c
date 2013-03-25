#include "mempool.h"

static struct list *head, *tail, *current;
static int mempool_size = 0, mempool_free = 0;
static int mempool_inited = 0;

static struct pool_entry *mem_pool_new_entry(int size)
{
	struct pool_entry * pentry = NULL;
	int size_align;

	if ( size <= 0 )
		return pentry;

	pentry = (struct pool_entry*) malloc(sizeof(struct pool_entry));
	if ( NULL == pentry )
		return pentry;

	/* 4字节对齐 */
	size_align = (size % 4 > 1) ? ((size + 4 ) / 4) : size;
	pentry->buff = malloc(size_align);
	if ( NULL == pentry->buff) {
		free(pentry);
		pentry = NULL;
		return pentry;
	}

	pentry->node.pre = pentry->node.next = NULL;
	pentry->node.data = pentry;
	pentry->total_size = size_align;
	pentry->inuse_size = 0;
	pentry->refcount = -1;

	mempool_size++;
	mempool_free++;
	
	return pentry;
}


static int mem_pool_insert(struct list *location, struct pool_entry *new_entry, int direct)
{
	int size;
	struct list *pcur, *new_node;
	struct pool_entry *tmp;
	int ret = -1;

	if ( NULL == location || 
			NULL == location->data ||
			NULL == new_entry )
		return ret;

	size = new_entry->total_size;
	new_node = &new_entry->node;
	pcur = location;
	tmp = pcur->data;

	switch (direct ) {
		case MEMPOOL_LITTLE:
little:
			if ( size >= tmp->total_size )
				goto big;
			while ( pcur && tmp->total_size >= size ) {
				pcur = pcur->pre;
				tmp = pcur->data;
			}
			
			if ( pcur && pcur != head ) {
				new_node->next = pcur->next;
				pcur->next = new_node;
				new_node->pre = pcur;
			} else {
				head->pre = new_node;
				new_node->next = head;
				head = new_node;
			}
			ret = 0;

			break;
		case MEMPOOL_BIG:
big:
			if ( size < tmp->total_size )
				goto little;
			
			while ( pcur && tmp->total_size < size ) {
				pcur = pcur->next;
				tmp = pcur->data;
			}

			if ( pcur && pcur != tail ) {
				new_node->pre = pcur->pre;
				pcur->pre = new_node;
				new_node->next = pcur;
			} else {
				tail->next = new_node;
				new_node->pre = tail;
				tail = new_node;
			}

			ret = 0;
			break;
		default:
			return ret;
	}

	return ret;
}

static void mem_pool_delete(struct pool_entry *pentry)
{

	struct list *pre, *next;
	if ( NULL == pentry )
		return;

	pre = pentry->node.pre;
	next = pentry->node.next;
	pre->next = next;
	next->pre = pre;	

	if ( NULL != pentry->buff )
		free(pentry->buff);
	free(pentry);
	mempool_size--;
}

static int mem_pool_init(int size)
{
	struct pool_entry * pentry;
	int ret = -1;

	if ( size <= 0 )
		return  ret;

	pentry = mem_pool_new_entry(size);
	if ( NULL == pentry )
		return ret;

	head = tail = current = &pentry->node;
	mempool_inited = 1;

	ret = 0;
	return ret;
}

static void entry_get(struct pool_entry *pentry)
{
	if ( NULL == pentry)
		return;
	if (!pentry->refcount++)
		mempool_free--;
}

static void entry_put(struct pool_entry *pentry)
{
	if ( NULL == pentry)
		return;
	
	if( pentry->refcount-- < 0) {
		if ( mempool_free++ > MEMPOOL_FREE_MAX ) {
			/* 内存池空闲条目数超过内存池最大空闲数目. 
			 * 如果内存块的大小很散列，单纯的使用最大空闲数目的方法
			 * 不能提高内存块的分配效率，所以这种方式适合于内存块大小
			 * 相差不大的情况。*/
			mem_pool_delete(pentry);
			mempool_free--;
		}
		pentry->inuse_size = 0;
		current = &pentry->node;
	}
}

/* 取得指定大小的内存块 */
int mem_pool_entry_get(struct pool_entry **pentry, int size)
{
	struct list *pcur, *pbest;
	struct list *insert_p = NULL;
	struct pool_entry *tmp;
	int current_total_size, found = 0, direct = 0;
	int ret = -1;
	
	if ( NULL == pentry || size < 0)
		return ret;
	
	if ( 0 == mempool_inited ) {
		ret = mem_pool_init(size);
		if ( -1 == ret )
			return ret;
	}

	tmp = current->data;
	current_total_size = tmp->total_size;
	if ( mempool_free ) {
		if ( current_total_size  > size ) {
			/* 需要申请的内存块大小比当前内存块要小 */
			pcur = current;
			do {
				if ( tmp->refcount == -1 ) {
					pbest = pcur;
					found = 1;
				}
				pcur = pcur->pre;
				if (pcur)
					tmp = pcur->data;
			}while( pcur && tmp->total_size > size );

			if ( pcur )
				insert_p = pcur->next;
			else 
				insert_p = head;

			if ( found ) {
				*pentry = (struct pool_entry *)pbest->data;
				entry_get(*pentry);
				(*pentry)->inuse_size = size;
				current = pbest;
				ret = 0;
				return ret;
			} else {
				/* 需要创建新的内存块 */	
				direct = MEMPOOL_LITTLE; 
				goto newentry;
			}
		}else {
			/* 需要申请的内存块大小比当前内存块要大(含等于) */

			pcur = current;
			while ( pcur && tmp->total_size < size ) {
				pcur = pcur->next;
				if ( pcur )
					tmp = pcur->data;
			}

			if ( pcur ) {
				insert_p = pcur->pre;
				while ( pcur ) {
					tmp = pcur->data;
					if ( tmp->refcount == -1 ) {
						pbest = pcur;
						found = 1;
						break;
					}
					pcur = pcur->next;
				}
			} else {
				insert_p = tail;
			}
			
			if ( found ) {
				*pentry = (struct pool_entry *)pbest->data;
				entry_get(*pentry);
				(*pentry)->inuse_size = size;
				current = pbest;
				ret = 0;
				return ret;
			} else {
				/* 需要新申请内存块 */
				direct = MEMPOOL_BIG;
				goto newentry;
			}
		}
	} 
newentry:
	tmp = mem_pool_new_entry(size);
	if ( NULL == tmp )
		return ret;

	if ( direct )
		mem_pool_insert(insert_p, tmp, direct); 
	else if ( tmp->total_size > current_total_size)
		mem_pool_insert(current, tmp, MEMPOOL_BIG);
	else
		mem_pool_insert(current, tmp, MEMPOOL_LITTLE);

	*pentry = tmp;
	entry_get(*pentry);
	current = &tmp->node;
	ret = 0;
	return ret;
}

void mem_pool_entry_put(struct pool_entry *pentry)
{
	if ( NULL == pentry )
		return;

	entry_put(pentry);
}

#ifdef __MEMPOOL_DEBUG_
void mem_pool_dump()
{
	struct list *node;
	struct pool_entry *pentry;
	int i=0;

	if ( mempool_inited ) {
		printf("memery pool: \ntotal: %d\nfree: %d\n", mempool_size, mempool_free);
		for ( node = head; node; node = node->next ) {
			pentry = (struct pool_entry *)node->data;
			printf("#%d total: %d, inuse_size: %d\n", i++, pentry->total_size,
					pentry->inuse_size);
		}
	}
}
#endif
