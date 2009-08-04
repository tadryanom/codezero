/*
 * Description of resources on the system
 *
 * Copyright (C) 2009 Bahadir Balban
 */

#ifndef __RESOURCES_H__
#define __RESOURCES_H__

/* Number of containers defined at compile-time */
#define CONFIG_TOTAL_CONTAINERS		1

#include <l4/generic/capability.h>
#include <l4/lib/idpool.h>
#include INC_SUBARCH(mm.h)

struct boot_resources {
	int nconts;
	int ncaps;
	int nids;
	int nthreads;
	int nspaces;
	int npmds;
	int nmutex;

	/* Kernel resource usage */
	int nkpmds;
	int nkpgds;
	int nkmemcaps;
};

/* List of containers */
struct container_head {
	int ncont;
	struct link list;
};

static inline void
container_head_init(struct container_head *chead)
{
	chead->ncont = 0;
	link_init(&chead->list);
}

/*
 * Everything on the platform is described and stored
 * in the structure below.
 */
struct kernel_container {
	l4id_t cid;

	/* System id pools */
	struct id_pool space_ids;
	struct id_pool ktcb_ids;
	struct id_pool resource_ids;
	struct id_pool container_ids;
	struct id_pool mutex_ids;
	struct id_pool capability_ids;

	/* List of all containers */
	struct container_head containers;

	/* Physical memory caps, used/unused */
	struct cap_list physmem_used;
	struct cap_list physmem_free;

	/* Virtual memory caps, used/unused */
	struct cap_list virtmem_used;
	struct cap_list virtmem_free;

	/* Device memory caps, used/unused */
	struct cap_list devmem_used;
	struct cap_list devmem_free;

	struct mem_cache *pgd_cache;
	struct mem_cache *pmd_cache;
	struct mem_cache *ktcb_cache;
	struct mem_cache *space_cache;
	struct mem_cache *mutex_cache;
	struct mem_cache *cap_cache;
	struct mem_cache *cont_cache;
};

extern struct kernel_container kernel_container;

void free_pgd(void *addr);
void free_pmd(void *addr);
void free_space(void *addr);
void free_ktcb(void *addr);
void free_capability(void *addr);
void free_container(void *addr);
void free_user_mutex(void *addr);

pgd_table_t *alloc_pgd(void);
pmd_table_t *alloc_pmd(void);
struct address_space *alloc_space(void);
struct ktcb *alloc_ktcb(void);
struct capability *alloc_capability(void);
struct container *alloc_container(void);
struct mutex_queue *alloc_user_mutex(void);

int init_system_resources(struct kernel_container *kcont);

#endif /* __RESOURCES_H__ */
