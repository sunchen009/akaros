/* See COPYRIGHT for copyright information. */
#ifdef __DEPUTY__
#pragma nodeputy
#endif

#include <arch/arch.h>
#include <arch/mmu.h>

#include <ros/error.h>

#include <atomic.h>
#include <string.h>
#include <assert.h>
#include <pmap.h>
#include <kclock.h>
#include <process.h>
#include <stdio.h>

//
// Allocate n bytes of physical memory aligned on an 
// align-byte boundary.  Align must be a power of two.
// Return kernel virtual address.  Returned memory is uninitialized.
//
// If we're out of memory, boot_alloc should panic.
// This function may ONLY be used during initialization,
// before the page_free_list has been set up.
// 
void*
boot_alloc(uint32_t n, uint32_t align)
{
	extern char end[];
	void *v;

	// Initialize boot_freemem if this is the first time.
	// 'end' is a magic symbol automatically generated by the linker,
	// which points to the end of the kernel's bss segment -
	// i.e., the first virtual address that the linker
	// did _not_ assign to any kernel code or global variables.
	if (boot_freemem == 0)
		boot_freemem = end;

	//	Step 1: round boot_freemem up to be aligned properly
	boot_freemem = ROUNDUP(boot_freemem, align);

	//	Step 2: save current value of boot_freemem as allocated chunk
	v = boot_freemem;
	//  Step 2.5: check if we can alloc
	if (PADDR(boot_freemem + n) > maxaddrpa)
		panic("Out of memory in boot alloc, you fool!\n");
	//	Step 3: increase boot_freemem to record allocation
	boot_freemem += n;	
	//	Step 4: return allocated chunk
	return v;
}

//
// Initialize a Page structure.
// The result has null links and 0 refcount.
// Note that the corresponding physical page is NOT initialized!
//
static void
page_initpp(page_t *pp)
{
	memset(pp, 0, sizeof(*pp));
}

/*
 * Allocates a physical page.
 * Does NOT set the contents of the physical page to zero -
 * the caller must do that if necessary.
 *
 * *pp_store   -- is set to point to the Page struct 
 *                of the newly allocated page
 *
 * RETURNS 
 *   0         -- on success
 *   -ENOMEM   -- otherwise 
 */
int page_alloc(page_t **pp_store)
{
	if (LIST_EMPTY(&page_free_list))
		return -ENOMEM;
	*pp_store = LIST_FIRST(&page_free_list);
	LIST_REMOVE(*pp_store, pp_link);
	page_initpp(*pp_store);
	return 0;
}

/*
 * Allocates a specific physical page.
 * Does NOT set the contents of the physical page to zero -
 * the caller must do that if necessary.
 *
 * *pp_store   -- is set to point to the Page struct 
 *                of the newly allocated page
 *
 * RETURNS 
 *   0         -- on success
 *   -ENOMEM   -- otherwise 
 */
int page_alloc_specific(page_t **pp_store, size_t ppn)
{
	page_t* page = ppn2page(ppn);
	if( page->pp_ref != 0 )
		return -ENOMEM;
	*pp_store = page;
	LIST_REMOVE(*pp_store, pp_link);
	page_initpp(*pp_store);
	return 0;
}

int page_is_free(size_t ppn) {
	page_t* page = ppn2page(ppn);
	if( page->pp_ref == 0 )
		return TRUE;
	return FALSE;
}

//
// Return a page to the free list.
// (This function should only be called when pp->pp_ref reaches 0.)
//
void page_free(page_t *pp)
{
	// this check allows us to call this on null ptrs, which helps when
	// allocating and checking for errors on several pages at once
	if (pp) {
		if (pp->pp_ref)
			panic("Attempting to free page with non-zero reference count!");
		LIST_INSERT_HEAD(&page_free_list, pp, pp_link);
	}
}

//
// Decrement the reference count on a page,
// freeing it if there are no more refs.
//
void
page_decref(page_t *pp)
{
	if (--pp->pp_ref == 0)
		page_free(pp);
}

//
// Map the physical page 'pp' at virtual address 'va'.
// The permissions (the low 12 bits) of the page table
//  entry should be set to 'perm|PTE_P'.
//
// Details
//   - If there is already a page mapped at 'va', it is page_remove()d.
//   - If necessary, on demand, allocates a page table and inserts it into
//     'pgdir'.
//   - pp->pp_ref should be incremented if the insertion succeeds.
//   - The TLB must be invalidated if a page was formerly present at 'va'.
//     (this is handled in page_remove)
//
// RETURNS: 
//   0 on success
//   -ENOMEM, if page table couldn't be allocated
//
// Hint: The TA solution is implemented using pgdir_walk, page_remove,
// and page2pa.
//
// No support for jumbos here.  will need to be careful of trying to insert
// regular pages into something that was already jumbo, and the overloading
// of the PTE_PS and PTE_PAT flags...
int
page_insert(pde_t *pgdir, page_t *pp, void *va, int perm) 
{
	pte_t* pte = pgdir_walk(pgdir, va, 1);
	if (!pte)
		return -ENOMEM;
	// need to up the ref count in case pp is already mapped at va
	// and we don't want to page_remove (which could free pp) and then 
	// continue as if pp wasn't freed.  moral = up the ref asap
	pp->pp_ref++;
	if (*pte & PTE_P) {
		page_remove(pgdir, va);
	}
	*pte = PTE(page2ppn(pp), PTE_P | perm);
	return 0;
}

//
// Map the physical page 'pp' at the first virtual address that is free 
// in the range 'vab' to 'vae'.
// The permissions (the low 12 bits) of the page table entry get set to 
// 'perm|PTE_P'.
//
// Details
//   - If there is no free entry in the range 'vab' to 'vae' this 
//     function returns -ENOMEM.
//   - If necessary, on demand, this function will allocate a page table 
//     and inserts it into 'pgdir'.
//   - pp->pp_ref should be incremented if the insertion succeeds.
//
// RETURNS: 
//   NULL, if no free va in the range (vab, vae) could be found
//   va,   the virtual address where pp has been mapped in the 
//         range (vab, vae)
//
void* page_insert_in_range(pde_t *pgdir, page_t *pp, 
                           void *vab, void *vae, int perm) 
{
	pte_t* pte = NULL;
	void* new_va;
	
	for(new_va = vab; new_va <= vae; new_va+= PGSIZE) {
		pte = pgdir_walk(pgdir, new_va, 1);
		if(pte != NULL && !(*pte & PTE_P)) break;
		else pte = NULL;
	}
	if (!pte) return NULL;
	*pte = page2pa(pp) | PTE_P | perm;
	return new_va;
}

//
// Return the page mapped at virtual address 'va'.
// If pte_store is not zero, then we store in it the address
// of the pte for this page.  This is used by page_remove
// but should not be used by other callers.
//
// Return 0 if there is no page mapped at va.
//
// Hint: the TA solution uses pgdir_walk and pa2page.
//
// For jumbos, right now this returns the first Page* in the 4MB
page_t *page_lookup(pde_t *pgdir, void *va, pte_t **pte_store)
{
	pte_t* pte = pgdir_walk(pgdir, va, 0);
	if (!pte || !(*pte & PTE_P))
		return 0;
	if (pte_store)
		*pte_store = pte;
	return pa2page(PTE_ADDR(*pte));
}

//
// Unmaps the physical page at virtual address 'va'.
// If there is no physical page at that address, silently does nothing.
//
// Details:
//   - The ref count on the physical page should decrement.
//   - The physical page should be freed if the refcount reaches 0.
//   - The pg table entry corresponding to 'va' should be set to 0.
//     (if such a PTE exists)
//   - The TLB must be invalidated if you remove an entry from
//     the pg dir/pg table.
//
// Hint: The TA solution is implemented using page_lookup,
// 	tlb_invalidate, and page_decref.
//
// This may be wonky wrt Jumbo pages and decref.  
void
page_remove(pde_t *pgdir, void *va)
{
	pte_t* pte;
	page_t *page;
	page = page_lookup(pgdir, va, &pte);
	if (!page)
		return;
	*pte = 0;
	tlb_invalidate(pgdir, va);
	page_decref(page);
}

//
// Invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
//
// Need to sort this for cross core lovin'  TODO
void
tlb_invalidate(pde_t *pgdir, void *va)
{
	// Flush the entry only if we're modifying the current address space.
	// For now, there is only one address space, so always invalidate.
	invlpg(va);
}

static void *DANGEROUS user_mem_check_addr;

//
// Check that an environment is allowed to access the range of memory
// [va, va+len) with permissions 'perm | PTE_P'.
// Normally 'perm' will contain PTE_U at least, but this is not required.
// 'va' and 'len' need not be page-aligned; you must test every page that
// contains any of that range.  You will test either 'len/PGSIZE',
// 'len/PGSIZE + 1', or 'len/PGSIZE + 2' pages.
//
// A user program can access a virtual address if (1) the address is below
// ULIM, and (2) the page table gives it permission.  These are exactly
// the tests you should implement here.
//
// If there is an error, set the 'user_mem_check_addr' variable to the first
// erroneous virtual address.
//
// Returns 0 if the user program can access this range of addresses,
// and -EFAULT otherwise.
//
// Hint: The TA solution uses pgdir_walk.
//

// zra: I've modified the interface to these two functions so that Ivy can
// check that user pointers aren't dereferenced. User pointers get the
// DANGEROUS qualifier. After validation, these functions return a
// COUNT(len) pointer. user_mem_check now returns NULL on error instead of
// -EFAULT.

void *COUNT(len)
user_mem_check(env_t *env, const void *DANGEROUS va, size_t len, int perm)
{
	// TODO - will need to sort this out wrt page faulting / PTE_P
	// also could be issues with sleeping and waking up to find pages
	// are unmapped, though i think the lab ignores this since the 
	// kernel is uninterruptible
	void *DANGEROUS start, *DANGEROUS end;
	size_t num_pages, i;
	pte_t *pte;

	perm |= PTE_P;
	start = ROUNDDOWN((void*)va, PGSIZE);
	end = ROUNDUP((void*)va + len, PGSIZE);
	if (start >= end) {
		warn("Blimey!  Wrap around in VM range calculation!");	
		return NULL;
	}
	num_pages = PPN(end - start);
	for (i = 0; i < num_pages; i++, start += PGSIZE) {
		pte = pgdir_walk(env->env_pgdir, start, 0);
		// ensures the bits we want on are turned on.  if not, error out
		if ( !pte || ((*pte & perm) != perm) ) {
			if (i = 0)
				user_mem_check_addr = (void*)va;
			else
				user_mem_check_addr = start;
			return NULL;
		}
	}
	// this should never be needed, since the perms should catch it
	if ((uintptr_t)end > ULIM) {
		warn ("I suck - Bug in user permission mappings!");
		return NULL;
	}
	return (void *COUNT(len))TC(va);
}

//
// Checks that environment 'env' is allowed to access the range
// of memory [va, va+len) with permissions 'perm | PTE_U'.
// If it can, then the function simply returns.
// If it cannot, 'env' is destroyed.
//
void *COUNT(len)
user_mem_assert(env_t *env, const void *DANGEROUS va, size_t len, int perm)
{
    void *COUNT(len) res = user_mem_check(env,va,len,perm | PTE_USER_RO);
	if (!res) {
		cprintf("[%08x] user_mem_check assertion failure for "
			"va %08x\n", env->env_id, user_mem_check_addr);
		proc_destroy(env);	// may not return
        return NULL;
	}
    return res;
}

// copies data from a user buffer to a kernel buffer.
// EFAULT if page not present, user lacks perms, or invalid addr.
error_t
memcpy_from_user(env_t* env, void* COUNT(len) dest,
                 const void *DANGEROUS va, size_t len)
{
	const void *DANGEROUS start, *DANGEROUS end;
	size_t num_pages, i;
	pte_t *pte;
	uintptr_t perm = PTE_P | PTE_USER_RO;
	size_t bytes_copied = 0;

	static_assert(ULIM % PGSIZE == 0 && ULIM != 0); // prevent wrap-around

	start = ROUNDDOWN(va, PGSIZE);
	end = ROUNDUP(va + len, PGSIZE);

	if(start >= (void*SNT)ULIM || end >= (void*SNT)ULIM)
		return -EFAULT;

	num_pages = PPN(end - start);
	for(i = 0; i < num_pages; i++)
	{
		pte = pgdir_walk(env->env_pgdir, start+i*PGSIZE, 0);
		if(!pte || (*pte & perm) != perm)
			return -EFAULT;

		void*COUNT(PGSIZE) kpage = KADDR(PTE_ADDR(pte));
		void* src_start = i > 0 ? kpage : kpage+(va-start);
		void* dst_start = dest+bytes_copied;
		size_t copy_len = PGSIZE;
		if(i == 0)
			copy_len -= va-start;
		if(i == num_pages-1)
			copy_len -= end-(start+len);

		memcpy(dst_start,src_start,copy_len);
		bytes_copied += copy_len;
	}

	assert(bytes_copied == len);

	return ESUCCESS;
}
