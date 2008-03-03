/*
 * Task management.
 *
 * Copyright (C) 2007 Bahadir Balban
 */
#include <l4/macros.h>
#include <l4/config.h>
#include <l4/types.h>
#include <l4/lib/list.h>
#include <l4/api/thread.h>
#include <l4/api/kip.h>
#include <l4/api/errno.h>
#include INC_GLUE(memory.h)
#include <l4lib/arch/syscalls.h>
#include <l4lib/arch/syslib.h>
#include <l4lib/arch/utcb.h>
#include <l4lib/ipcdefs.h>
#include <lib/addr.h>
#include <task.h>
#include <kdata.h>
#include <kmalloc/kmalloc.h>
#include <string.h>
#include <vm_area.h>
#include <memory.h>
#include <file.h>
#include <utcb.h>
#include <proc.h>

struct tcb_head {
	struct list_head list;
	int total;			/* Total threads */
} tcb_head;

struct tcb *find_task(int tid)
{
	struct tcb *t;

	list_for_each_entry(t, &tcb_head.list, list)
		if (t->tid == tid)
			return t;
	return 0;
}

#if 0
void dump_tasks(void)
{
	struct tcb *t;

	list_for_each_entry(t, &tcb_head.list, list) {
		printf("Task %s: id/spid: %d/%d\n", &t->name[0], t->tid, t->spid);
		printf("Task vm areas:\n");
		dump_vm_areas(t);
		printf("Task swapfile:\n");
		dump_task_swapfile(t);
	}
}
#endif


struct tcb *create_init_tcb(struct tcb_head *tcbs)
{
	struct tcb *task = kzalloc(sizeof(struct tcb));

	/* Ids will be acquired from the kernel */
	task->tid = TASK_ID_INVALID;
	task->spid = TASK_ID_INVALID;
	INIT_LIST_HEAD(&task->list);
	INIT_LIST_HEAD(&task->vm_area_list);
	list_add_tail(&task->list, &tcbs->list);
	tcbs->total++;

	/* Allocate a utcb virtual address */
	task->utcb_address = (unsigned long)utcb_vaddr_new();

	return task;
}

int start_boot_tasks(struct initdata *initdata, struct tcb_head *tcbs)
{
	int err;
	struct vm_file *file;
	struct svc_image *img;
	unsigned int sp, pc;
	struct tcb *task;
	struct task_ids ids;
	struct bootdesc *bd = initdata->bootdesc;

	INIT_LIST_HEAD(&tcb_head.list);
	INIT_LIST_HEAD(&initdata->boot_file_list);

	for (int i = 0; i < bd->total_images; i++) {
		img = &bd->images[i];

		/* Skip self */
		if (!strcmp(img->name, __PAGERNAME__))
			continue;

		/* Set up task ids */
		if (!strcmp(img->name, __VFSNAME__)) {
			ids.tid = VFS_TID;
			ids.spid = VFS_TID;
		} else {
			ids.tid = -1;
			ids.spid = -1;
		}

		printf("Creating new thread.\n");
		/* Create the thread structures and address space */
		if ((err = l4_thread_control(THREAD_CREATE, &ids)) < 0) {
			printf("l4_thread_control failed with %d.\n", err);
			goto error;
		}

		/* Create a task and use returned space and thread ids. */
		printf("New task with id: %d, space id: %d\n", ids.tid, ids.spid);
		task = create_init_tcb(tcbs);
		task->tid = ids.tid;
		task->spid = ids.spid;

		/*
		 * For boot files, we use the physical address of the memory
		 * file as its mock-up inode.
		 */
		file = vmfile_alloc_init();
		file->vnum = img->phys_start;
		file->length = img->phys_end - img->phys_start;
		file->pager = &boot_file_pager;
		list_add(&file->list, &initdata->boot_file_list);

		/* Prepare environment boundaries. Posix minimum is 4Kb */
		task->env_end = USER_AREA_END;
		task->env_start = task->env_end - PAGE_SIZE;
		task->args_start = task->env_start;
		task->args_end = task->env_start;

		/*
		 * Prepare the task environment file and data.
		 * Currently it only has the utcb address. The env pager
		 * when faulted, simply copies the task env data to the
		 * allocated page.
		 */
		if (task_prepare_proc_files(task) < 0) {
			printf("Could not create environment file.\n");
			goto error;
		}

		/*
		 * Task stack starts right after the environment,
		 * and is of 4 page size.
		 */
		task->stack_end = task->env_start;
		task->stack_start = task->stack_end - PAGE_SIZE * 4;

		/* Currently RO text and RW data are one region */
		task->data_start = USER_AREA_START;
		task->data_end = USER_AREA_START + file->length;
		task->text_start = task->data_start;
		task->text_end = task->data_end;

		/* Set up task's registers */
		sp = align(task->stack_end - 1, 8);
		pc = task->text_start;

		/* mmap each task's physical image to task's address space. */
		if ((err = do_mmap(file, 0, task, USER_AREA_START,
				   VM_READ | VM_WRITE,
				   __pfn(page_align_up(file->length)))) < 0) {
			printf("do_mmap: failed with %d.\n", err);
			goto error;
		}

		/* mmap each task's environment from its env file. */
		if ((err = do_mmap(task->proc_files->env_file, 0, task,
				   task->env_start, VM_READ | VM_WRITE,
				   __pfn(task->env_end - task->env_start)) < 0)) {
			printf("do_mmap: Mapping environment failed with %d.\n",
			       err);
			goto error;
		}

		/* mmap each task's stack as 4-page anonymous memory. */
		if ((err = do_mmap(0, 0, task, task->stack_start,
				   VM_READ | VM_WRITE | VMA_ANON,
				   __pfn(task->stack_end - task->stack_start)) < 0)) {
			printf("do_mmap: Mapping stack failed with %d.\n", err);
			goto error;
		}

		/* mmap each task's utcb as single page anonymous memory. */
		printf("%s: Mapping utcb for new task at: 0x%x\n", __TASKNAME__,
		       task->utcb_address);
		if ((err = do_mmap(0, 0, task, task->utcb_address,
				   VM_READ | VM_WRITE | VMA_ANON, 1) < 0)) {
			printf("do_mmap: Mapping utcb failed with %d.\n", err);
			goto error;
		}

		/* Set up the task's thread details, (pc, sp, pager etc.) */
		if ((err = l4_exchange_registers(pc, sp, self_tid(), task->tid) < 0)) {
			printf("l4_exchange_registers failed with %d.\n", err);
			goto error;
		}

		printf("Starting task with id %d\n", task->tid);

		/* Start the thread */
		if ((err = l4_thread_control(THREAD_RUN, &ids) < 0)) {
			printf("l4_thread_control failed with %d\n", err);
			goto error;
		}
	}
	return 0;

error:
	BUG();
}


void init_pm(struct initdata *initdata)
{
	start_boot_tasks(initdata, &tcb_head);
}

/*
 * Makes the virtual to page translation for a given user task.
 */
struct page *task_virt_to_page(struct tcb *t, unsigned long virtual)
{
	unsigned long vaddr_vma_offset;
	unsigned long vaddr_file_offset;
	struct vm_area *vma;
	struct vm_file *vmfile;
	struct page *page;

	/* First find the vma that maps that virtual address */
	if (!(vma = find_vma(virtual, &t->vm_area_list))) {
		printf("%s: No VMA found for 0x%x on task: %d\n",
		       __FUNCTION__, virtual, t->tid);
		return PTR_ERR(-EINVAL);
	}

	/* Find the pfn offset of virtual address in this vma */
	BUG_ON(__pfn(virtual) < vma->pfn_start ||
	       __pfn(virtual) > vma->pfn_end);
	vaddr_vma_offset = __pfn(virtual) - vma->pfn_start;

	/* Find the file offset of virtual address in this file */
	vmfile = vma->owner;
	vaddr_file_offset = vma->f_offset + vaddr_vma_offset;

	/*
	 * Find the page with the same file offset with that of the
	 * virtual address, that is, if the page is resident in memory.
	 */
	list_for_each_entry(page, &vmfile->page_cache_list, list)
		if (vaddr_file_offset == page->f_offset) {
			printf("%s: %s: Found page @ 0x%x, f_offset: 0x%x, with vma @ 0x%x, vmfile @ 0x%x\n", __TASKNAME__,
			       __FUNCTION__, (unsigned long)page, page->f_offset, vma, vma->owner);
			return page;
		}

	/*
	 * The page is not found, meaning that it is not mapped in
	 * yet, e.g. via a page fault.
	 */
	return 0;
}

struct task_data {
	unsigned long tid;
	unsigned long utcb_address;
};

struct task_data_head {
	unsigned long total;
	struct task_data tdata[];
};

/*
 * During its initialisation FS0 wants to learn how many boot tasks
 * are running, and their tids, which includes itself. This function
 * provides that information.
 */
void send_task_data(l4id_t requester)
{
	int li, err;
	struct tcb *t, *vfs;
	struct utcb *vfs_utcb;
	struct task_data_head *tdata_head;

	if (requester != VFS_TID) {
		printf("%s: Task data requested by %d, which is not "
		       "FS0 id %d, ignoring.\n", __TASKNAME__, requester,
		       VFS_TID);
		return;
	}

	BUG_ON(!(vfs = find_task(requester)));

	/* Map in vfs's utcb. FIXME: Whatif it is already mapped? */
	l4_map((void *)page_to_phys(task_virt_to_page(vfs, vfs->utcb_address)),
	       (void *)vfs->utcb_address, 1, MAP_USR_RW_FLAGS, self_tid());

	/* Get a handle on vfs utcb */
	vfs_utcb = (struct utcb *)vfs->utcb_address;

	/* Write all requested task information to utcb's user buffer area */
	tdata_head = (struct task_data_head *)vfs_utcb->buf;

	/* First word is total number of tcbs */
	tdata_head->total = tcb_head.total;

	/* Write per-task data for all tasks */
	li = 0;
	list_for_each_entry(t, &tcb_head.list, list) {
		tdata_head->tdata[li].tid = t->tid;
		tdata_head->tdata[li].utcb_address = t->utcb_address;
		li++;
	}

	/* Reply */
	if ((err = l4_ipc_return(0)) < 0) {
		printf("%s: L4 IPC Error: %d.\n", __FUNCTION__, err);
		BUG();
	}
}

