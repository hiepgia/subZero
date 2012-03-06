/* xcfmalloc.c */

/*
 * xcfmalloc is a memory allocator based on a scatter-gather model that
 * allows for cross-page storage and relocatable data blocks.  This is
 * achieved through a simple, though non-standard, memory allocation API.
 *
 * xcfmalloc is a replacement for the xvmalloc allocator used by zcache
 * for persistent compressed page storage.
 *
 * xcfmalloc uses a scatter-gather block model in order to reduce external
 * fragmentation (at the expense of an extra memcpy).  Low fragmentation
 * is especially important for a zcache allocator since memory will already
 * be under pressure.
 *
 * It also uses relocatable data blocks in order to allow compaction to take
 * place.  Compacting seeks to move blocks to other, less sparsely used,
 * pages such that the maximum number of pages can be returned to the kernel
 * buddy allocator.  NOTE: Compaction is not yet implemented.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "xcfmalloc.h"

/* turn debugging on */
#define XCFM_DEBUG

/* tunables */
#define XCF_BLOCK_SHIFT 6
#define XCF_MAX_RESERVE_PAGES 32
#define XCF_MAX_BLOCKS_PER_ALLOC 2

#define XCF_BLOCK_ALIGN (1 << XCF_BLOCK_SHIFT)
#define XCF_NUM_FREELISTS (PAGE_SIZE >> XCF_BLOCK_SHIFT)
#define XCF_RESERVED_INDEX (XCF_NUM_FREELISTS - 1)

static inline int xcf_align_to_block(int size)
{
	return ALIGN(size, XCF_BLOCK_ALIGN);
}

static inline int xcf_size_to_flindex(int size)
{

	return (xcf_align_to_block(size) >> XCF_BLOCK_SHIFT) - 1;
}

/* pool structure */
struct xcf_pool {
	struct xcf_blkdesc *freelists[XCF_NUM_FREELISTS];
	spinlock_t lock;
	gfp_t flags; /* flags used for page allocations */
	int reserve_count; /* number of reserved (completely unused) pages */
	int page_count; /* number of pages used and reserved */
	int desc_count; /* number of block descriptors */
};

/* block descriptor structure and helper functions */
struct xcf_blkdesc {
	struct page *page;
	struct xcf_blkdesc *next;
	struct xcf_blkdesc *prev;
	int offxcf_set_flags;
#define XCF_FLAG_MASK (XCF_BLOCK_ALIGN - 1)
#define XCF_OFFSET_MASK (~XCF_FLAG_MASK)
#define XCF_BLOCK_FREE (1<<0)
#define XCF_BLOCK_USED (1<<1)
	/* size means different things depending on whether the block is
	 * in use or not. If the block is free, the size is the aligned size
	 * of the block and the block can contain an allocation of up to
	 * (size - sizeof(struct xcf_blkhdr)). If the block is in use,
	 * size is the actual number of bytes used by the allocation plus
	 * sizeof(struct xcf_blkhdr) */
	int size;
};

static inline void xcf_set_offset(struct xcf_blkdesc *desc, int offset)
{
	BUG_ON(offset != (offset & XCF_OFFSET_MASK));
	desc->offxcf_set_flags = offset |
		(desc->offxcf_set_flags & XCF_FLAG_MASK);
}

static inline int xcf_get_offset(struct xcf_blkdesc *desc)
{
	return desc->offxcf_set_flags & XCF_OFFSET_MASK;
}

static inline void xcf_set_flags(struct xcf_blkdesc *desc, int flags)
{
	BUG_ON(flags != (flags & XCF_FLAG_MASK));
	desc->offxcf_set_flags = flags |
		(desc->offxcf_set_flags & XCF_OFFSET_MASK);
}

static inline bool xcf_is_free(struct xcf_blkdesc *desc)
{
	return desc->offxcf_set_flags & XCF_BLOCK_FREE;
}

static inline bool xcf_is_used(struct xcf_blkdesc *desc)
{
	return desc->offxcf_set_flags & XCF_BLOCK_USED;
}

/* block header structure and helper functions */
#ifdef XCFM_DEBUG
#define DECL_SENTINEL int sentinel
#define SENTINEL 0xdeadbeef
#define SET_SENTINEL(_obj) ((_obj)->sentinel = SENTINEL)
#define ASSERT_SENTINEL(_obj) BUG_ON((_obj)->sentinel != SENTINEL)
#define CLEAR_SENTINEL(_obj) ((_obj)->sentinel = 0)
#else
#define DECL_SENTINEL
#define SET_SENTINEL(_obj) do { } while (0)
#define ASSERT_SENTINEL(_obj) do { } while (0)
#define CLEAR_SENTINEL(_obj) do { } while (0)
#endif

struct xcf_blkhdr {
	struct xcf_blkdesc *desc;
	int prevoffset;
	DECL_SENTINEL;
};

#define MAX_ALLOC_SIZE (PAGE_SIZE - sizeof(struct xcf_blkhdr))

static inline void *xcf_page_start(struct xcf_blkhdr *block)
{
	return (void *)((unsigned long)block & PAGE_MASK);
}

static inline struct xcf_blkhdr *xcf_block_offset(struct xcf_blkhdr *block,
						int offset)
{
	return (struct xcf_blkhdr *)((char *)block + offset);
}

static inline bool xcf_same_page(struct xcf_blkhdr *block1,
				struct xcf_blkhdr *block2)
{
	return (xcf_page_start(block1) == xcf_page_start(block2));
}

static inline void *__xcf_map_block(struct xcf_blkdesc *desc,
					enum km_type type)
{
	return kmap_atomic(desc->page, type) + xcf_get_offset(desc);
}

static inline void *xcf_map_block(struct xcf_blkdesc *desc, enum km_type type)
{
	struct xcf_blkhdr *block;
	block = __xcf_map_block(desc, type);
	ASSERT_SENTINEL(block);
	return block;
}

static inline void xcf_unmap_block(struct xcf_blkhdr *block, enum km_type type)
{
	kunmap_atomic(xcf_page_start(block), type);
}

/* descriptor memory cache */
static struct kmem_cache *xcf_desc_cache;

/* descriptor management */

struct xcf_blkdesc *xcf_alloc_desc(struct xcf_pool *pool, gfp_t flags)
{
	struct xcf_blkdesc *desc;
	desc = kmem_cache_zalloc(xcf_desc_cache, flags);
	if (desc)
		pool->desc_count++;
	return desc;
}

void xcf_free_desc(struct xcf_pool *pool, struct xcf_blkdesc *desc)
{
	kmem_cache_free(xcf_desc_cache, desc);
	pool->desc_count--;
}

/* block management */

static void xcf_remove_block(struct xcf_pool *pool, struct xcf_blkdesc *desc)
{
	u32 flindex;

	BUG_ON(!xcf_is_free(desc));
	flindex = xcf_size_to_flindex(desc->size);
	if (flindex == XCF_RESERVED_INDEX)
		pool->reserve_count--;
	if (pool->freelists[flindex] == desc)
		pool->freelists[flindex] = desc->next;
	if (desc->prev)
		desc->prev->next = desc->next;
	if (desc->next)
		desc->next->prev = desc->prev;
	desc->next = NULL;
	desc->prev = NULL;
}

static void xcf_insert_block(struct xcf_pool *pool, struct xcf_blkdesc *desc)
{
	u32 flindex;

	flindex = xcf_size_to_flindex(desc->size);
	if (flindex == XCF_RESERVED_INDEX)
		pool->reserve_count++;
	/* the block size needs to be realigned since it was previously
	 * set to the actual allocation size */
	desc->size = xcf_align_to_block(desc->size);
	desc->next = pool->freelists[flindex];
	desc->prev = NULL;
	xcf_set_flags(desc, XCF_BLOCK_FREE);
	pool->freelists[flindex] = desc;
	if (desc->next)
		desc->next->prev = desc;
}

/*
 * The xcf_find_remove_block function contains the block selection logic for
 * this allocator.
 *
 * This selection pattern has the advantage of utilizing blocks in partially
 * used pages before considering a completely unused page.  This results
 * in high utilization of the pool pages and the expense of higher allocation
 * fragmentation (i.e. more blocks per allocation).  This is acceptable
 * though since, at this point, memory is the resource under pressure.
*/
static struct xcf_blkdesc *xcf_find_remove_block(struct xcf_pool *pool,
	int size, int blocknum)
{
	int flindex, i;
	struct xcf_blkdesc *desc = NULL;

	flindex = xcf_size_to_flindex(size + sizeof(struct xcf_blkhdr));

	/* look for best fit */
	if (pool->freelists[flindex])
		goto remove;

	/* if this is the last block allowed in the allocation, we shouldn't
	 * consider smaller blocks.  it's all or nothing now */
	if (blocknum == XCF_MAX_BLOCKS_PER_ALLOC) {
		/* look for largest smaller block */
		for (i = flindex; i > 0; i--) {
			if (pool->freelists[i]) {
				flindex = i;
				goto remove;
			}
		}
	}

	/* look for smallest larger block */
	for (i = flindex + 1; i < XCF_NUM_FREELISTS; i++) {
		if (pool->freelists[i]) {
			flindex = i;
			goto remove;
		}
	}

	/* if we get here, there are no blocks that satisfy the request */
	return NULL;

remove:
	desc = pool->freelists[flindex];
	xcf_remove_block(pool, desc);
	return desc;
}

struct xcf_blkdesc *xcf_merge_free_block(struct xcf_pool *pool,
					struct xcf_blkdesc *desc)
{
	struct xcf_blkdesc *next, *prev;
	struct xcf_blkhdr *block, *nextblock, *prevblock;

	block = xcf_map_block(desc, KM_USER0);

	prevblock = xcf_block_offset(xcf_page_start(block), block->prevoffset);
	if (xcf_get_offset(desc) == 0) {
		prev = NULL;
	} else {
		ASSERT_SENTINEL(prevblock);
		prev = prevblock->desc;
	}

	nextblock = xcf_block_offset(block, desc->size);
	if (!xcf_same_page(block, nextblock)) {
		next = NULL;
	} else {
		ASSERT_SENTINEL(nextblock);
		next = nextblock->desc;
	}

	/* merge adjacent free blocks in page */
	if (prev && xcf_is_free(prev)) {
		/* merge with previous block */
		xcf_remove_block(pool, prev);
		prev->size += desc->size;
		xcf_free_desc(pool, desc);
		CLEAR_SENTINEL(block);
		desc = prev;
		block = prevblock;
	}

	if (next && xcf_is_free(next)) {
		/* merge with next block */
		xcf_remove_block(pool, next);
		desc->size += next->size;
		CLEAR_SENTINEL(nextblock);
		nextblock = xcf_block_offset(nextblock, next->size);
		xcf_free_desc(pool, next);
		if (!xcf_same_page(nextblock, block)) {
			next = NULL;
		} else {
			ASSERT_SENTINEL(nextblock);
			next = nextblock->desc;
		}
	}

	if (next)
		nextblock->prevoffset = xcf_get_offset(desc);

	xcf_unmap_block(block, KM_USER0);

	return desc;
}

/* pool management */

/* try to get pool->reserve_count up to XCF_MAX_RESERVE_PAGES */
static void xcf_increase_pool(struct xcf_pool *pool, gfp_t flags)
{
	struct xcf_blkdesc *desc;
	int deficit_pages;
	struct xcf_blkhdr *block;
	struct page *page;

	/* if we are at or above our desired number of
	 * reserved pages, nothing to do */
	if (pool->reserve_count >= XCF_MAX_RESERVE_PAGES)
		return;

	/* alloc some pages (if we can) */
	deficit_pages = XCF_MAX_RESERVE_PAGES - pool->reserve_count;
	while (deficit_pages--) {
		desc = xcf_alloc_desc(pool, GFP_NOWAIT);
		if (!desc)
			break;
		/* we use our own scatter-gather mechanism that maps high
		 * memory pages under the covers, so we add HIGHMEM
		 * even if the caller did not explicitly request it */
		page = alloc_page(flags | __GFP_HIGHMEM);
		if (!page) {
			xcf_free_desc(pool, desc);
			break;
		}
		pool->page_count++;
		desc->page = page;
		xcf_set_offset(desc, 0);
		desc->size = PAGE_SIZE;
		xcf_insert_block(pool, desc);
		block = __xcf_map_block(desc, KM_USER0);
		block->desc = desc;
		block->prevoffset = 0;
		SET_SENTINEL(block);
		xcf_unmap_block(block, KM_USER0);
	}
}

/* tries to get pool->reserve_count down to XCF_MAX_RESERVE_PAGES */
static void xcf_decrease_pool(struct xcf_pool *pool)
{
	struct xcf_blkdesc *desc;
	int surplus_pages;

	/* if we are at are below our desired number of
	 * reserved pages, nothing to do */
	if (pool->reserve_count <= XCF_MAX_RESERVE_PAGES)
		return;

	/* free some pages */
	surplus_pages = pool->reserve_count - XCF_MAX_RESERVE_PAGES;
	while (surplus_pages--) {
		desc = pool->freelists[XCF_RESERVED_INDEX];
		BUG_ON(!desc);
		xcf_remove_block(pool, desc);
		__free_page(desc->page);
		pool->page_count--;
		xcf_free_desc(pool, desc);
	}
}

/* public functions */

/* return handle to new descect with specified size */
void *xcf_malloc(struct xcf_pool *pool, int size, gfp_t flags)
{
	struct xcf_blkdesc *first, *prev, *next, *desc, *splitdesc;
	int i, sizeleft, alignedleft;
	struct xcf_blkhdr *block, *nextblock, *splitblock;

	if (size > MAX_ALLOC_SIZE)
		return NULL;

	spin_lock(&pool->lock);

	/* check and possibly increase (alloc) pool's page reserves */
	xcf_increase_pool(pool, flags);

	sizeleft = size;
	first = NULL;
	prev = NULL;

	/* find block(s) to contain the allocation */
	for (i = 1; i <= XCF_MAX_BLOCKS_PER_ALLOC; i++) {
		desc = xcf_find_remove_block(pool, sizeleft, i);
		if (!desc)
			goto return_blocks;
		if (!first)
			first = desc;
		desc->prev = prev;
		if (prev)
			prev->next = desc;
		xcf_set_flags(desc, XCF_BLOCK_USED);
		if (desc->size >= sizeleft + sizeof(struct xcf_blkhdr))
			break;
		sizeleft -= desc->size - sizeof(struct xcf_blkhdr);
		prev = desc;
	}

	/* split is only possible on the last block */
	alignedleft = xcf_align_to_block(sizeleft + sizeof(struct xcf_blkhdr));
	if (desc->size > alignedleft) {
		splitdesc = xcf_alloc_desc(pool, GFP_NOWAIT);
		if (!splitdesc)
			goto return_blocks;
		splitdesc->size = desc->size - alignedleft;
		splitdesc->page = desc->page;
		xcf_set_offset(splitdesc, xcf_get_offset(desc) + alignedleft);

		block = xcf_map_block(desc, KM_USER0);
		ASSERT_SENTINEL(block);
		nextblock = xcf_block_offset(block, desc->size);
		if (xcf_same_page(nextblock, block))
			nextblock->prevoffset = xcf_get_offset(splitdesc);
		splitblock = xcf_block_offset(block, alignedleft);
		splitblock->desc = splitdesc;
		splitblock->prevoffset = xcf_get_offset(desc);
		SET_SENTINEL(splitblock);
		xcf_unmap_block(block, KM_USER0);

		xcf_insert_block(pool, splitdesc);
	}

	desc->size = sizeleft + sizeof(struct xcf_blkhdr);

	/* ensure the changes to desc are written before a potential
	 read in xcf_get_alloc_size() since it does not aquire a
	 lock */
	wmb();

	spin_unlock(&pool->lock);
	return first;

return_blocks:
	desc = first;
	while (desc) {
		next = desc->next;
		xcf_insert_block(pool, desc);
		desc = next;
	}
	spin_unlock(&pool->lock);
	return NULL;
}

enum xcf_op {
	XCFMOP_READ,
	XCFMOP_WRITE
};

static int access_allocation(void *handle, char *buf, enum xcf_op op)
{
	struct xcf_blkdesc *desc;
	int count, offset;
	char *block;

	desc = handle;
	count = XCF_MAX_BLOCKS_PER_ALLOC;
	offset = 0;
	while (desc && count--) {
		BUG_ON(!xcf_is_used(desc));
		BUG_ON(offset > MAX_ALLOC_SIZE);

		block = xcf_map_block(desc, KM_USER0);
		if (op == XCFMOP_WRITE)
			memcpy(block + sizeof(struct xcf_blkhdr),
				buf + offset,
				desc->size - sizeof(struct xcf_blkhdr));
		else /* XCFMOP_READ */
			memcpy(buf + offset,
				block + sizeof(struct xcf_blkhdr),
				desc->size - sizeof(struct xcf_blkhdr));
		xcf_unmap_block((struct xcf_blkhdr *)block, KM_USER0);

		offset += desc->size - sizeof(struct xcf_blkhdr);
		desc = desc->next;
	}

	BUG_ON(desc);

	return 0;
}

/* write data from buf into allocation */
int xcf_write(struct xcf_handle *handle, void *buf)
{
	return access_allocation(handle, buf, XCFMOP_WRITE);
}

/* read data from allocation into buf */
int xcf_read(struct xcf_handle *handle, void *buf)
{
	return access_allocation(handle, buf, XCFMOP_READ);
}

/* free an allocation */
void xcf_free(struct xcf_pool *pool, struct xcf_handle *handle)
{
	struct xcf_blkdesc *desc, *nextdesc;
	int count;

	spin_lock(&pool->lock);

	desc = (struct xcf_blkdesc *)handle;
	count = XCF_MAX_BLOCKS_PER_ALLOC;
	while (desc && count--) {
		nextdesc = desc->next;
		BUG_ON(xcf_is_free(desc));
		xcf_set_flags(desc, XCF_BLOCK_FREE);
		desc->size = xcf_align_to_block(desc->size);
		/* xcf_merge_free_block may merge with a previous block which
		 * will cause desc to change */
		desc = xcf_merge_free_block(pool, desc);
		xcf_insert_block(pool, desc);
		desc = nextdesc;
	}

	BUG_ON(desc);

	/* check and possibly decrease (free) pool's page reserves */
	xcf_decrease_pool(pool);

	spin_unlock(&pool->lock);
}

/* create an xcfmalloc memory pool */
struct xcf_pool *xcf_create_pool(gfp_t flags)
{
	struct xcf_pool *pool = NULL;

	if (!xcf_desc_cache)
		xcf_desc_cache = kmem_cache_create("xcf_desc_cache",
			sizeof(struct xcf_blkdesc), 0, 0, NULL);

	if (!xcf_desc_cache)
		goto out;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		goto out;

	spin_lock_init(&pool->lock);

	xcf_increase_pool(pool, flags);

out:
	return pool;
}

/* destroy an xcfmalloc memory pool */
void xcf_destroy_pool(struct xcf_pool *pool)
{
	struct xcf_blkdesc *desc, *nextdesc;

	/* free reserved pages */
	desc = pool->freelists[XCF_RESERVED_INDEX];
	while (desc) {
		nextdesc = desc->next;
		__free_page(desc->page);
		xcf_free_desc(pool, desc);
		desc = nextdesc;
	}

	kfree(pool);
}

/* get size of allocation associated with handle */
int xcf_get_alloc_size(struct xcf_handle *handle)
{
	struct xcf_blkdesc *desc;
	int count, size = 0;

	if (!handle)
		goto out;

	/* ensure the changes to desc by xcf_malloc() are written before
	 we access here since we don't acquire a lock */
	rmb();

	desc = (struct xcf_blkdesc *)handle;
	count = XCF_MAX_BLOCKS_PER_ALLOC;
	while (desc && count--) {
		BUG_ON(!xcf_is_used(desc));
		size += desc->size - sizeof(struct xcf_blkhdr);
		desc = desc->next;
	}

out:
	return size;
}

/* get total number of pages in use by pool not including the reserved pages */
u64 xcf_get_total_size_bytes(struct xcf_pool *pool)
{
	u64 ret;
	spin_lock(&pool->lock);
	ret = pool->page_count - pool->reserve_count;
	spin_unlock(&pool->lock);
	return ret << PAGE_SHIFT;
}

/* get number of descriptors in use by pool not including the descriptors for
 * reserved pages */
int xcf_get_desc_count(struct xcf_pool *pool)
{
	int ret;
	spin_lock(&pool->lock);
	ret = pool->desc_count - pool->reserve_count;
	spin_unlock(&pool->lock);
	return ret;
}

