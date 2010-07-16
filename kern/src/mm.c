/* Copyright (c) 2009, 2010 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Virtual memory management functions.  Creation, modification, etc, of virtual
 * memory regions (VMRs) as well as mmap(), mprotect(), and munmap().
 *
 * In general, error checking / bounds checks are done in the main function
 * (e.g. mmap()), and the work is done in a do_ function (e.g. do_mmap()).
 * Versions of those functions that are called when the memory lock (proc_lock
 * for now) is already held begin with __ (e.g. __do_munmap()).  */

#include <frontend.h>
#include <ros/common.h>
#include <ros/mman.h>
#include <pmap.h>
#include <mm.h>
#include <process.h>
#include <stdio.h>
#include <syscall.h>
#include <slab.h>
#include <kmalloc.h>
#include <vfs.h>

struct kmem_cache *vmr_kcache;

void vmr_init(void)
{
	vmr_kcache = kmem_cache_create("vm_regions", sizeof(struct vm_region),
	                               __alignof__(struct dentry), 0, 0, 0);
}

/* For now, the caller will set the prot, flags, file, and offset.  In the
 * future, we may put those in here, to do clever things with merging vm_regions
 * that are the same.
 *
 * TODO: take a look at solari's vmem alloc.  And consider keeping these in a
 * tree of some sort for easier lookups. */
struct vm_region *create_vmr(struct proc *p, uintptr_t va, size_t len)
{
	struct vm_region *vmr = 0, *vm_i, *vm_next;
	uintptr_t gap_end;

	assert(!PGOFF(va));
	assert(!PGOFF(len));
	assert(va + len <= UMAPTOP);

	/* Is there room before the first one: */
	vm_i = TAILQ_FIRST(&p->vm_regions);
	if (!vm_i || (va + len < vm_i->vm_base)) {
		vmr = kmem_cache_alloc(vmr_kcache, 0);
		if (!vmr)
			panic("EOM!");
		vmr->vm_base = va;
		TAILQ_INSERT_HEAD(&p->vm_regions, vmr, vm_link);
	} else {
		TAILQ_FOREACH(vm_i, &p->vm_regions, vm_link) {
			vm_next = TAILQ_NEXT(vm_i, vm_link);
			gap_end = vm_next ? vm_next->vm_base : UMAPTOP;
			/* skip til we get past the 'hint' va */
			if (va >= gap_end)
				continue;
			/* Find a gap that is big enough */
			if (gap_end - vm_i->vm_end >= len) {
				vmr = kmem_cache_alloc(vmr_kcache, 0);
				/* if we can put it at va, let's do that.  o/w, put it so it
				 * fits */
				if ((gap_end >= va + len) && (va >= vm_i->vm_end))
					vmr->vm_base = va;
				else
					vmr->vm_base = vm_i->vm_end;
				TAILQ_INSERT_AFTER(&p->vm_regions, vm_i, vmr, vm_link);
				break;
			}
		}
	}
	/* Finalize the creation, if we got one */
	if (vmr) {
		vmr->vm_proc = p;
		vmr->vm_end = vmr->vm_base + len;
	}
	if (!vmr)
		warn("Not making a VMR, wanted %08p, + %p = %p", va, len, va + len);
	return vmr;
}

/* Split a VMR at va, returning the new VMR.  It is set up the same way, with
 * file offsets fixed accordingly.  'va' is the beginning of the new one, and
 * must be page aligned. */
struct vm_region *split_vmr(struct vm_region *old_vmr, uintptr_t va)
{
	struct vm_region *new_vmr;

	assert(!PGOFF(va));
	if ((old_vmr->vm_base >= va) || (old_vmr->vm_end <= va))
		return 0;
	new_vmr = kmem_cache_alloc(vmr_kcache, 0);
	TAILQ_INSERT_AFTER(&old_vmr->vm_proc->vm_regions, old_vmr, new_vmr,
	                   vm_link);
	new_vmr->vm_proc = old_vmr->vm_proc;
	new_vmr->vm_base = va;
	new_vmr->vm_end = old_vmr->vm_end;
	old_vmr->vm_end = va;
	new_vmr->vm_prot = old_vmr->vm_prot;
	new_vmr->vm_flags = old_vmr->vm_flags;
	if (old_vmr->vm_file) {
		new_vmr->vm_file = old_vmr->vm_file;
		atomic_inc(&new_vmr->vm_file->f_refcnt);
		new_vmr->vm_foff = old_vmr->vm_foff +
		                      old_vmr->vm_end - old_vmr->vm_base;
	} else {
		new_vmr->vm_file = 0;
		new_vmr->vm_foff = 0;
	}
	return new_vmr;
}

/* Merges two vm regions.  For now, it will check to make sure they are the
 * same.  The second one will be destroyed. */
int merge_vmr(struct vm_region *first, struct vm_region *second)
{
	assert(first->vm_proc == second->vm_proc);
	if ((first->vm_end != second->vm_base) ||
	    (first->vm_prot != second->vm_prot) ||
	    (first->vm_flags != second->vm_flags) ||
	    (first->vm_file != second->vm_file))
		return -1;
	if ((first->vm_file) && (second->vm_foff != first->vm_foff +
	                         first->vm_end - first->vm_base))
		return -1;
	first->vm_end = second->vm_end;
	destroy_vmr(second);
	return 0;
}

/* Attempts to merge vmr with adjacent VMRs, returning a ptr to be used for vmr.
 * It could be the same struct vmr, or possibly another one (usually lower in
 * the address space. */
struct vm_region *merge_me(struct vm_region *vmr)
{
	struct vm_region *vmr_temp;
	/* Merge will fail if it cannot do it.  If it succeeds, the second VMR is
	 * destroyed, so we need to be a bit careful. */
	vmr_temp = TAILQ_PREV(vmr, vmr_tailq, vm_link);
	if (vmr_temp)
		if (!merge_vmr(vmr_temp, vmr))
			vmr = vmr_temp;
	vmr_temp = TAILQ_NEXT(vmr, vm_link);
	if (vmr_temp)
		merge_vmr(vmr, vmr_temp);
	return vmr;
}

/* Grows the vm region up to (and not including) va.  Fails if another is in the
 * way, etc. */
int grow_vmr(struct vm_region *vmr, uintptr_t va)
{
	assert(!PGOFF(va));
	struct vm_region *next = TAILQ_NEXT(vmr, vm_link);
	if (next && next->vm_base < va)
		return -1;
	if (va <= vmr->vm_end)
		return -1;
	vmr->vm_end = va;
	return 0;
}

/* Shrinks the vm region down to (and not including) va.  Whoever calls this
 * will need to sort out the page table entries. */
int shrink_vmr(struct vm_region *vmr, uintptr_t va)
{
	assert(!PGOFF(va));
	if ((va < vmr->vm_base) || (va > vmr->vm_end))
		return -1;
	vmr->vm_end = va;
	return 0;
}

/* Called by the unmapper, just cleans up.  Whoever calls this will need to sort
 * out the page table entries. */
void destroy_vmr(struct vm_region *vmr)
{
	if (vmr->vm_file)
		atomic_dec(&vmr->vm_file->f_refcnt);
	TAILQ_REMOVE(&vmr->vm_proc->vm_regions, vmr, vm_link);
	kmem_cache_free(vmr_kcache, vmr);
}

/* Given a va and a proc (later an mm, possibly), returns the owning vmr, or 0
 * if there is none. */
struct vm_region *find_vmr(struct proc *p, uintptr_t va)
{
	struct vm_region *vmr;
	/* ugly linear seach */
	TAILQ_FOREACH(vmr, &p->vm_regions, vm_link) {
		if ((vmr->vm_base <= va) && (vmr->vm_end > va))
			return vmr;
	}
	return 0;
}

/* Finds the first vmr after va (including the one holding va), or 0 if there is
 * none. */
struct vm_region *find_first_vmr(struct proc *p, uintptr_t va)
{
	struct vm_region *vmr;
	/* ugly linear seach */
	TAILQ_FOREACH(vmr, &p->vm_regions, vm_link) {
		if ((vmr->vm_base <= va) && (vmr->vm_end > va))
			return vmr;
		if (vmr->vm_base > va)
			return vmr;
	}
	return 0;
}

/* Makes sure that no VMRs cross either the start or end of the given region
 * [va, va + len), splitting any VMRs that are on the endpoints. */
void isolate_vmrs(struct proc *p, uintptr_t va, size_t len)
{
	struct vm_region *vmr;
	if ((vmr = find_vmr(p, va)))
		split_vmr(vmr, va);
	/* TODO: don't want to do another find (linear search) */
	if ((vmr = find_vmr(p, va + len)))
		split_vmr(vmr, va + len);
}

/* This will make new_p have the same VMRs as p, though it does nothing to
 * ensure the physical pages or whatever are shared/mapped/copied/whatever.
 * This is used by fork().
 *
 * Note that if you are working on a VMR that is a file, you'll want to be
 * careful about how it is mapped (SHARED, PRIVATE, etc). */
void duplicate_vmrs(struct proc *p, struct proc *new_p)
{
	struct vm_region *vmr, *vm_i;
	TAILQ_FOREACH(vm_i, &p->vm_regions, vm_link) {
		vmr = kmem_cache_alloc(vmr_kcache, 0);
		if (!vmr)
			panic("EOM!");
		vmr->vm_proc = new_p;
		vmr->vm_base = vm_i->vm_base;
		vmr->vm_end = vm_i->vm_end;
		vmr->vm_prot = vm_i->vm_prot;	
		vmr->vm_flags = vm_i->vm_flags;	
		vmr->vm_file = vm_i->vm_file;
		if (vmr->vm_file)
			atomic_inc(&vmr->vm_file->f_refcnt);
		vmr->vm_foff = vm_i->vm_foff;
		TAILQ_INSERT_TAIL(&new_p->vm_regions, vmr, vm_link);
	}
}

void print_vmrs(struct proc *p)
{
	int count = 0;
	struct vm_region *vmr;
	printk("VM Regions for proc %d\n", p->pid);
	TAILQ_FOREACH(vmr, &p->vm_regions, vm_link)
		printk("%02d: (0x%08x - 0x%08x): %08p, %08p, %08p\n", count++,
		       vmr->vm_base, vmr->vm_end, vmr->vm_prot, vmr->vm_flags,
		       vmr->vm_file);
}


/* Error values aren't quite comprehensive - check man mmap() once we do better
 * with the FS */
void *mmap(struct proc *p, uintptr_t addr, size_t len, int prot, int flags,
           int fd, size_t offset)
{
	struct file *file = NULL;
	printd("mmap(addr %x, len %x, prot %x, flags %x, fd %x, off %x)\n", addr,
	       len, prot, flags, fd, offset);
	if (fd >= 0 && (flags & MAP_SHARED)) {
		printk("[kernel] mmap() for files requires !MAP_SHARED.\n");
		set_errno(current_tf, EACCES);
		return MAP_FAILED;
	}
	if (fd >= 0 && (flags & MAP_ANON)) {
		set_errno(current_tf, EBADF);
		return MAP_FAILED;
	}
	if ((addr + len > UMAPTOP) || (PGOFF(addr))) {
		set_errno(current_tf, EINVAL);
		return MAP_FAILED;
	}
	if (!len) {
		set_errno(current_tf, EINVAL);
		return MAP_FAILED;
	}
	if (fd != -1) {
		file = file_open_from_fd(p, fd);
		if (!file) {
			set_errno(current_tf, EBADF);
			return MAP_FAILED;
		}
	}
	addr = MAX(addr, MMAP_LOWEST_VA);
	void *result = do_mmap(p, addr, len, prot, flags, file, offset);
	if (file)
		file_decref(file);
	return result;
}

void *do_mmap(struct proc *p, uintptr_t addr, size_t len, int prot, int flags,
              struct file *file, size_t offset)
{
	// TODO: grab the appropriate mm_lock
	spin_lock(&p->proc_lock);
	void *ret = __do_mmap(p, addr, len, prot, flags, file, offset);
	spin_unlock(&p->proc_lock);
	return ret;
}

void *__do_mmap(struct proc *p, uintptr_t addr, size_t len, int prot, int flags,
                struct file *file, size_t offset)
{
	len = ROUNDUP(len, PGSIZE);
	int num_pages = len / PGSIZE;

	struct vm_region *vmr, *vmr_temp;

#ifndef __CONFIG_DEMAND_PAGING__
	flags |= MAP_POPULATE;
#endif
	/* Need to make sure nothing is in our way when we want a FIXED location.
	 * We just need to split on the end points (if they exist), and then remove
	 * everything in between.  __do_munmap() will do this. */
	if (flags & MAP_FIXED)
		__do_munmap(p, addr, len);
	vmr = create_vmr(p, addr, len);
	if (!vmr) {
		printk("[kernel] do_mmap() aborted for %08p + %d!\n", addr, len);
		set_errno(current_tf, ENOMEM);
		return MAP_FAILED;		/* TODO: error propagation for mmap() */
	}
	vmr->vm_prot = prot;
	vmr->vm_flags = flags;
	vmr->vm_file = file;
	vmr->vm_foff = offset;
	addr = vmr->vm_base;		/* so we know which pages to populate later */
	vmr = merge_me(vmr);		/* attempts to merge with neighbors */
	/* Fault in pages now if MAP_POPULATE - die on failure.  We want to populate
	 * the region requested, but we need to be careful and only populate the
	 * requested length and not any merged regions, which is why we set addr
	 * above and use it here. */
	if (flags & MAP_POPULATE)
		for (int i = 0; i < num_pages; i++)
			if (__handle_page_fault(p, addr + i*PGSIZE, vmr->vm_prot)) {
				spin_unlock(&p->proc_lock);
				proc_destroy(p);
			}
	return (void*SAFE)TC(addr);
}

int mprotect(struct proc *p, uintptr_t addr, size_t len, int prot)
{
	printd("mprotect(addr %x, len %x, prot %x)\n", addr, len, prot);
	if (!len)
		return 0;
	if ((addr % PGSIZE) || (addr < MMAP_LOWEST_VA)) {
		set_errno(current_tf, EINVAL);
		return -1;
	}
	uintptr_t end = ROUNDUP(addr + len, PGSIZE);
	if (end > UMAPTOP || addr > end) {
		set_errno(current_tf, ENOMEM);
		return -1;
	}
	spin_lock(&p->proc_lock);
	int ret = __do_mprotect(p, addr, len, prot);
	spin_unlock(&p->proc_lock);
	return ret;
}

/* This does not care if the region is not mapped.  POSIX says you should return
 * ENOMEM if any part of it is unmapped.  Can do this later if we care, based on
 * the VMRs, not the actual page residency. */
int __do_mprotect(struct proc *p, uintptr_t addr, size_t len, int prot)
{
	struct vm_region *vmr, *next_vmr;
	pte_t *pte;
	bool shootdown_needed = FALSE;
	int pte_prot = (prot & PROT_WRITE) ? PTE_USER_RW :
	               (prot & (PROT_READ|PROT_EXEC)) ? PTE_USER_RO : 0;
	/* TODO: this is aggressively splitting, when we might not need to if the
	 * prots are the same as the previous.  Plus, there are three excessive
	 * scans.  Finally, we might be able to merge when we are done. */
	isolate_vmrs(p, addr, addr + len);
	vmr = find_first_vmr(p, addr);
	while (vmr && vmr->vm_base < addr + len) {
		if (vmr->vm_prot == prot)
			continue;
		/* if vmr maps a file, then we need to make sure the protection change
		 * is in compliance with the open mode of the file.  At least for any
		 * mapping that is write-backed to a file.  For now, we just do it for
		 * all file mappings.  And this hasn't been tested */
		if (vmr->vm_file && (prot & PROT_WRITE)) {
			if (!(vmr->vm_file->f_mode & PROT_WRITE)) {
				set_errno(current_tf, EACCES);
				return -1;
			}
		}
		vmr->vm_prot = prot;
		for (uintptr_t va = vmr->vm_base; va < vmr->vm_end; va += PGSIZE) { 
			pte = pgdir_walk(p->env_pgdir, (void*)va, 0);
			if (pte && PAGE_PRESENT(*pte)) {
				*pte = (*pte & ~PTE_PERM) | pte_prot;
				shootdown_needed = TRUE;
			}
		}
		next_vmr = TAILQ_NEXT(vmr, vm_link);
		vmr = next_vmr;
	}
	if (shootdown_needed)
		__proc_tlbshootdown(p, addr, addr + len);
	return 0;
}

int munmap(struct proc *p, uintptr_t addr, size_t len)
{
	printd("munmap(addr %x, len %x, prot %x)\n", addr, len, prot);
	if (!len)
		return 0;
	if ((addr % PGSIZE) || (addr < MMAP_LOWEST_VA)) {
		set_errno(current_tf, EINVAL);
		return -1;
	}
	uintptr_t end = ROUNDUP(addr + len, PGSIZE);
	if (end > UMAPTOP || addr > end) {
		set_errno(current_tf, EINVAL);
		return -1;
	}
	spin_lock(&p->proc_lock);
	int ret = __do_munmap(p, addr, len);
	spin_unlock(&p->proc_lock);
	return ret;
}

int __do_munmap(struct proc *p, uintptr_t addr, size_t len)
{
	struct vm_region *vmr, *next_vmr;
	pte_t *pte;
	bool shootdown_needed = FALSE;

	/* TODO: this will be a bit slow, since we end up doing three linear
	 * searches (two in isolate, one in find_first). */
	isolate_vmrs(p, addr, addr + len);
	vmr = find_first_vmr(p, addr);
	while (vmr && vmr->vm_base < addr + len) {
		for (uintptr_t va = vmr->vm_base; va < vmr->vm_end; va += PGSIZE) { 
			pte = pgdir_walk(p->env_pgdir, (void*)va, 0);
			if (!pte)
				continue;
			if (PAGE_PRESENT(*pte)) {
				/* TODO: (TLB) race here, where the page can be given out before
				 * the shootdown happened.  Need to put it on a temp list. */
				page_t *page = ppn2page(PTE2PPN(*pte));
				*pte = 0;
				page_decref(page);
				shootdown_needed = TRUE;
			} else if (PAGE_PAGED_OUT(*pte)) {
				/* TODO: (SWAP) mark free in the swapfile or whatever.  For now,
				 * PAGED_OUT is also being used to mean "hasn't been mapped
				 * yet".  Note we now allow PAGE_UNMAPPED, unlike older
				 * versions of mmap(). */
				panic("Swapping not supported!");
				*pte = 0;
			}
		}
		next_vmr = TAILQ_NEXT(vmr, vm_link);
		destroy_vmr(vmr);
		vmr = next_vmr;
	}
	if (shootdown_needed)
		__proc_tlbshootdown(p, addr, addr + len);
	return 0;
}

int handle_page_fault(struct proc* p, uintptr_t va, int prot)
{
	va = ROUNDDOWN(va,PGSIZE);

	if (prot != PROT_READ && prot != PROT_WRITE && prot != PROT_EXEC)
		panic("bad prot!");

	spin_lock(&p->proc_lock);
	int ret = __handle_page_fault(p, va, prot);
	spin_unlock(&p->proc_lock);
	return ret;
}

/* Returns 0 on success, or an appropriate -error code.  Assumes you hold the
 * appropriate lock.
 *
 * Notes: if your TLB caches negative results, you'll need to flush the
 * appropriate tlb entry.  Also, you could have a weird race where a present PTE
 * faulted for a different reason (was mprotected on another core), and the
 * shootdown is on its way.  Userspace should have waited for the mprotect to
 * return before trying to write (or whatever), so we don't care and will fault
 * them. */
int __handle_page_fault(struct proc* p, uintptr_t va, int prot)
{
	struct vm_region *vmr;
	struct page *a_page;
	unsigned int f_idx;	/* index of the missing page in the file */
	int retval = 0;
	/* Check the vmr's protection */
	vmr = find_vmr(p, va);
	if (!vmr)							/* not mapped at all */
		return -EFAULT;
	if (!(vmr->vm_prot & prot))			/* wrong prots for this vmr */
		return -EFAULT;
	/* find offending PTE (prob don't read this in).  This might alloc an
	 * intermediate page table page. */
	pte_t *pte = pgdir_walk(p->env_pgdir, (void*)va, 1);
	if (!pte)
		return -ENOMEM;
	/* a spurious, valid PF is possible due to a legit race: the page might have
	 * been faulted in by another core already (and raced on the memory lock),
	 * in which case we should just return. */
	if (PAGE_PRESENT(*pte)) {
		return 0;
	} else if (PAGE_PAGED_OUT(*pte)) {
		/* TODO: (SWAP) bring in the paged out frame. (BLK) */
		panic("Swapping not supported!");
		return 0;
	}
	if (!vmr->vm_file) {
		/* No file - just want anonymous memory */
		if (upage_alloc(p, &a_page, TRUE))
			return -ENOMEM;
	} else {
		/* Load the file's page in the page cache.
		 * TODO: (BLK) Note, we are holding the mem lock!  We need to rewrite
		 * this stuff so we aren't hold the lock as excessively as we are, and
		 * such that we can block and resume later. */
		f_idx = (va - vmr->vm_base + vmr->vm_foff) >> PGSHIFT;
		retval = file_load_page(vmr->vm_file, f_idx, &a_page);
		if (retval)
			return retval;
		/* if this is an executable page, we might have to flush the instruction
		 * cache if our HW requires it. */
		if (vmr->vm_prot & PROT_EXEC)
			icache_flush_page((void*)va, page2kva(a_page));
	}
	/* update the page table TODO: careful with MAP_PRIVATE etc.  might do this
	 * separately (file, no file) */
	int pte_prot = (vmr->vm_prot & PROT_WRITE) ? PTE_USER_RW :
	               (vmr->vm_prot & (PROT_READ|PROT_EXEC)) ? PTE_USER_RO : 0;
	page_incref(a_page);	/* incref, since we manually insert in the pgdir */
	*pte = PTE(page2ppn(a_page), PTE_P | pte_prot);
	return 0;
}
