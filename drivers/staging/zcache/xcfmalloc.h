/* xcfmalloc.h */

#ifndef _XCFMALLOC_H_
#define _XCFMALLOC_H_

#include <linux/kernel.h>
#include <linux/types.h>

struct xcf_pool;
struct xcf_handle {
	void *desc;
};

void *xcf_malloc(struct xcf_pool *pool, int size, gfp_t flags);
void xcf_free(struct xcf_pool *pool, struct xcf_handle *handle);

int xcf_write(struct xcf_handle *handle, void *buf);
int xcf_read(struct xcf_handle *handle, void *buf);

struct xcf_pool *xcf_create_pool(gfp_t flags);
void xcf_destroy_pool(struct xcf_pool *pool);

int xcf_get_alloc_size(struct xcf_handle *handle);

u64 xcf_get_total_size_bytes(struct xcf_pool *pool);
int xcf_get_desc_count(struct xcf_pool *pool);

#endif /* _XCFMALLOC_H_ */

