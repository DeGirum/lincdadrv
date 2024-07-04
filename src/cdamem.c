// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2020 Egor Pomozov.
//
// Originally memalloc sequence was designed for simple driver
// in Aquantia Corp by Vadim Solomin 
// Later was updated by QA team in Aquantia Corp.
// Later it was additionally modifyied by Egor Pomozov
// 
// CDA linux driver memory request handler
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//

#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>

#include "cdadrv.h"
#include "cdaioctl.h"

#include <linux/version.h>


#if LINUX_VERSION_CODE < KERNEL_VERSION(5,6,0)
/**
 * pin_user_pages_fast() - pin user pages in memory without taking locks
 *
 * For now, this is a placeholder function, until various call sites are
 * converted to use the correct get_user_pages*() or pin_user_pages*() API. So,
 * this is identical to get_user_pages_fast().
 *
 * This is intended for Case 1 (DIO) in Documentation/vm/pin_user_pages.rst. It
 * is NOT intended for Case 2 (RDMA: long-term pins).
 */
static int pin_user_pages_fast(unsigned long start, int nr_pages,
			unsigned int gup_flags, struct page **pages)
{
	/*
	 * This is a placeholder, until the pin functionality is activated.
	 * Until then, just behave like the corresponding get_user_pages*()
	 * routine.
	 */
	return get_user_pages_fast(start, nr_pages, gup_flags, pages);
}

/**
 * unpin_user_page() - release a gup-pinned page
 * @page:            pointer to page to be released
 *
 * Pages that were pinned via pin_user_pages*() must be released via either
 * unpin_user_page(), or one of the unpin_user_pages*() routines. This is so
 * that eventually such pages can be separately tracked and uniquely handled. In
 * particular, interactions with RDMA and filesystems need special handling.
 *
 * unpin_user_page() and put_page() are not interchangeable, despite this early
 * implementation that makes them look the same. unpin_user_page() calls must
 * be perfectly matched up with pin*() calls.
 */
static inline void unpin_user_page(struct page *page)
{
	put_page(page);
}

/**
 * unpin_user_pages() - release an array of gup-pinned pages.
 * @pages:  array of pages to be marked dirty and released.
 * @npages: number of pages in the @pages array.
 *
 * For each page in the @pages array, release the page using unpin_user_page().
 *
 * Please see the unpin_user_page() documentation for details.
 */
static void unpin_user_pages(struct page **pages, unsigned long npages)
{
	unsigned long index;

	/*
	 * TODO: this can be optimized for huge pages: if a series of pages is
	 * physically contiguous and part of the same compound page, then a
	 * single operation to the head page should suffice.
	 */
	for (index = 0; index < npages; index++)
		unpin_user_page(pages[index]);
}

/**
 * unpin_user_pages_dirty_lock() - release and optionally dirty gup-pinned pages
 * @pages:  array of pages to be maybe marked dirty, and definitely released.
 * @npages: number of pages in the @pages array.
 * @make_dirty: whether to mark the pages dirty
 *
 * "gup-pinned page" refers to a page that has had one of the get_user_pages()
 * variants called on that page.
 *
 * For each page in the @pages array, make that page (or its head page, if a
 * compound page) dirty, if @make_dirty is true, and if the page was previously
 * listed as clean. In any case, releases all pages using unpin_user_page(),
 * possibly via unpin_user_pages(), for the non-dirty case.
 *
 * Please see the unpin_user_page() documentation for details.
 *
 * set_page_dirty_lock() is used internally. If instead, set_page_dirty() is
 * required, then the caller should a) verify that this is really correct,
 * because _lock() is usually required, and b) hand code it:
 * set_page_dirty_lock(), unpin_user_page().
 *
 */
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages,
				 bool make_dirty)
{
	unsigned long index;

	/*
	 * TODO: this can be optimized for huge pages: if a series of pages is
	 * physically contiguous and part of the same compound page, then a
	 * single operation to the head page should suffice.
	 */

	if (!make_dirty) {
		unpin_user_pages(pages, npages);
		return;
	}

	for (index = 0; index < npages; index++) {
		struct page *page = compound_head(pages[index]);
		/*
		 * Checking PageDirty at this point may race with
		 * clear_page_dirty_for_io(), but that's OK. Two key
		 * cases:
		 *
		 * 1) This code sees the page as already dirty, so it
		 * skips the call to set_page_dirty(). That could happen
		 * because clear_page_dirty_for_io() called
		 * page_mkclean(), followed by set_page_dirty().
		 * However, now the page is going to get written back,
		 * which meets the original intention of setting it
		 * dirty, so all is well: clear_page_dirty_for_io() goes
		 * on to call TestClearPageDirty(), and write the page
		 * back.
		 *
		 * 2) This code sees the page as clean, so it calls
		 * set_page_dirty(). The page stays dirty, despite being
		 * written back, so it gets written back again in the
		 * next writeback cycle. This is harmless.
		 */
		if (!PageDirty(page))
			set_page_dirty_lock(page);
		unpin_user_page(page);
	}
}

#endif

static ssize_t name_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct cda_dev *cdadev = container_of((dev), struct cda_dev, dev);
	return sprintf(buf, "cda%d\n", cdadev->minor);
}
static DEVICE_ATTR_RO(name);

static struct attribute *cda_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};

static struct attribute_group cda_attr_grp = {
	.attrs = cda_attrs,
};

static ssize_t mblk_attr_show(
	struct kobject *kobj, 
	struct attribute *attr,
	char *buf);

static void mblk_release(struct kobject *kobj);

struct cda_mblk {
	struct cda_dev *dev;
	int index;

	struct kobject kobj;
	uint32_t req_size;
	void *vaddr; //kernel
	uint32_t size;
	dma_addr_t paddr;
	void *owner;
	struct list_head list;
	struct bin_attribute mmap_attr;
};

struct cda_mmap {
	struct cda_dev *dev;
	int index;

	struct kobject kobj;
	void *owner;

	void *vaddr; //original user
	uint32_t size; //original user
	uint32_t blk_cnt;
	uint32_t mapped_blk_cnt;
	uint32_t show_cnt;
	struct sg_table sgt;
	struct page **pages;
	struct cda_drv_sg_item *sg_list;
	struct list_head list;
	struct bin_attribute mmap_attr;
};

struct mblkitem_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct cda_mblk *, char *);
	ssize_t (*store)(struct cda_mblk *, char*, size_t);
};

#define cda_dev_mblk_attr(_field, _fmt)					\
	static ssize_t							\
	mblk_##_field##_show(struct cda_mblk *mblk, char *buf)	\
	{								\
		return sprintf(buf, _fmt, mblk->_field);		\
	}								\
	static struct mblkitem_sysfs_entry mblk_##_field##_attr =	\
		__ATTR(_field, S_IRUGO, mblk_##_field##_show, NULL);

#pragma GCC diagnostic ignored "-Wformat"
cda_dev_mblk_attr(vaddr, "0x%lx\n");
cda_dev_mblk_attr(paddr, "0x%lx\n");
cda_dev_mblk_attr(size, "0x%x\n");
cda_dev_mblk_attr(req_size, "0x%x\n");
cda_dev_mblk_attr(owner, "0x%p\n");
cda_dev_mblk_attr(index, "%d\n");
#pragma GCC diagnostic warning "-Wformat"

static struct attribute *mblk_attrs[] = {
	&mblk_vaddr_attr.attr,
	&mblk_paddr_attr.attr,
	&mblk_size_attr.attr,
	&mblk_owner_attr.attr,
	&mblk_req_size_attr.attr,
	&mblk_index_attr.attr,
	NULL,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
ATTRIBUTE_GROUPS(mblk);
#endif
static const struct sysfs_ops mblk_ops = {
	.show = mblk_attr_show,
};

struct kobj_type mblk_type = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
	.default_groups = mblk_groups,
#else
	.default_attrs = mblk_attrs,
#endif
	.sysfs_ops = &mblk_ops,
	.release = mblk_release,
};

static ssize_t mblk_attr_show(struct kobject *kobj, 
	struct attribute *attr, char *buf)
{
	struct cda_mblk *mblk = container_of(kobj, struct cda_mblk, kobj);
	struct mblkitem_sysfs_entry *entry =
		container_of(attr, struct mblkitem_sysfs_entry, attr);

	if (!entry->show)
		return -EIO;

	return entry->show(mblk, buf);
}

static void mblk_release(struct kobject *kobj)
{
	struct cda_mblk *mblk = container_of(kobj, struct cda_mblk, kobj);
	kfree(mblk);
}

#define to_memmap(obj) container_of(obj, struct cda_mmap, kobj)

struct memmapitem_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct cda_mmap *, char *);
	ssize_t (*store)(struct cda_mmap *, char *, size_t);
};

#define cda_dev_memmap_attr(_field, _fmt)					\
	static ssize_t							\
	memmap_##_field##_show(struct cda_mmap *memmap, char *buf)	\
	{								\
		return sprintf(buf, _fmt, memmap->_field);		\
	}								\
	static struct memmapitem_sysfs_entry memmap_##_field##_attr =	\
		__ATTR(_field, S_IRUGO, memmap_##_field##_show, NULL);

#pragma GCC diagnostic ignored "-Wformat"
cda_dev_memmap_attr(owner, "0x%p\n");
cda_dev_memmap_attr(vaddr, "0x%lx\n");
cda_dev_memmap_attr(size, "0x%x\n");
cda_dev_memmap_attr(index, "%d\n");
cda_dev_memmap_attr(blk_cnt, "%d\n");

static ssize_t
memmap_sglist_show(struct cda_mmap *memmap, char *buf)
{
	const int sg_list_item_size = 16 + 8 + 2; //"%016llx %08lx\n"
	int res = 0;
	int i = memmap->show_cnt;
	memmap->show_cnt = 0;
	buf[0] = '\0';
	for( ; i < memmap->blk_cnt; i++ ) {
		if( (res + sg_list_item_size) >= (PAGE_SIZE - 1)) /* https://lwn.net/Articles/178634/ */{
			memmap->show_cnt = i;
			//printk("Split SG list. Next read starts with item: %d\n", i);
			break;
		}
		res += sprintf(&buf[res], "%016llx %08lx\n", memmap->sg_list[i].paddr, memmap->sg_list[i].size);
	}
	return res;
}

static struct memmapitem_sysfs_entry memmap_sglist_attr =
	__ATTR(sglist, S_IRUGO, memmap_sglist_show, NULL);

#pragma GCC diagnostic warning "-Wformat"
static struct attribute *memmap_attrs[] = {
	&memmap_owner_attr.attr,
	&memmap_vaddr_attr.attr,
	&memmap_size_attr.attr,
	&memmap_index_attr.attr,
	&memmap_blk_cnt_attr.attr,
	&memmap_sglist_attr.attr,
	NULL,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
ATTRIBUTE_GROUPS(memmap);
#endif

static ssize_t memmap_attr_show(struct kobject *kobj, 
	struct attribute *attr, char *buf)
{
	struct cda_mmap *memmap = to_memmap(kobj);
	struct memmapitem_sysfs_entry *entry =
		container_of(attr, struct memmapitem_sysfs_entry, attr);

	if (!entry->show)
		return -EIO;

	return entry->show(memmap, buf);
}

static void memmap_release(struct kobject *kobj)
{
	struct cda_mmap *memmap = to_memmap(kobj);
	kfree(memmap);
}

static const struct sysfs_ops memmap_ops = {
	.show = memmap_attr_show,
};

struct kobj_type memmap_type = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
	.default_groups = memmap_groups,
#else
	.default_attrs = memmap_attrs,
#endif
	.sysfs_ops = &memmap_ops,
	.release = memmap_release,
};

static int mblk_mmap( struct file *file, 
						struct kobject *kobj, 
						struct bin_attribute *attr,
			   			struct vm_area_struct *vma)
{
	struct cda_mblk *mblk = attr->private;
	unsigned long requested = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	unsigned long pages = (unsigned long)mblk->req_size >> PAGE_SHIFT;

	if (vma->vm_pgoff + requested > pages)
		return -EINVAL;

	if( dma_mmap_coherent(  &mblk->dev->pcidev->dev,
							vma,
							mblk->vaddr,
							mblk->paddr,
							mblk->req_size) )
	{
		dev_err(&mblk->dev->pcidev->dev, "DMA remapping failed");
		return -ENXIO;
	}
	return 0;
}

void cda_hide_memmap(struct cda_mmap *memmap);
int cda_publish_memmap(struct cda_mmap *memmap);
void cda_hide_mblk(struct cda_mblk *mblk);
int cda_publish_mblk(struct cda_mblk *mblk);

int cda_publish_mblk(struct cda_mblk *mblk)
{
	int ret;
	struct bin_attribute *mmap_attr = &mblk->mmap_attr;

	ret = kobject_add(  &mblk->kobj, mblk->dev->kobj_mems,
						"%04d", mblk->index);
	if (ret)
		goto err_add;

	mmap_attr->mmap = mblk_mmap;
	mmap_attr->attr.name = "mmap";
	mmap_attr->attr.mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
	mmap_attr->size = mblk->req_size;
	mmap_attr->private = mblk;
	ret = sysfs_create_bin_file(&mblk->kobj, mmap_attr);
	if (ret)
		goto err_map_add;

	return 0;

err_map_add:
	kobject_del(&mblk->kobj);
err_add:
	kobject_put(&mblk->kobj);
	return ret;
}


void cda_hide_mblk(struct cda_mblk *mblk)
{
	sysfs_remove_bin_file(&mblk->kobj, &mblk->mmap_attr);
	kobject_del(&mblk->kobj);
}

int cda_publish_memmap(struct cda_mmap *memmap)
{
	int ret;
	struct bin_attribute *mmap_attr = &memmap->mmap_attr;

	ret = kobject_add(  &memmap->kobj, memmap->dev->kobj_mems,
						"%04d", memmap->index);
	if (ret)
		goto err_add;

	mmap_attr->attr.name = "memmapobj";
	mmap_attr->attr.mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
	mmap_attr->size = memmap->size;
	mmap_attr->private = memmap;
	ret = sysfs_create_bin_file(&memmap->kobj, mmap_attr);
	if (ret)
		goto err_map_add;

	return 0;

err_map_add:
	kobject_del(&memmap->kobj);
err_add:
	kobject_put(&memmap->kobj);
	return ret;
}

void cda_hide_memmap(struct cda_mmap *memmap)
{
	sysfs_remove_bin_file(&memmap->kobj, &memmap->mmap_attr);
	kobject_del(&memmap->kobj);
}

int cda_alloc_mem(struct cda_dev *dev, void *owner, void __user *ureq)
{
	int ret = -ENOMEM;
	int idx;
	struct cda_mblk *mblk;
	struct cda_alloc_mem req;
	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;

	mblk = kzalloc(sizeof(*mblk), in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	if (!mblk) {
		dev_err(&dev->dev, "Can't alloc mblk\n");
		goto out;
	}
	INIT_LIST_HEAD(&mblk->list);
	mblk->dev = dev;
	kobject_init(&mblk->kobj, &mblk_type);
	mblk->owner = owner;
	mblk->size = req.size;
	req.size = ALIGN(req.size, PAGE_SIZE);

	idr_preload(in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	spin_lock(&dev->mblk_sl);
	ret = idr_alloc(&dev->mblk_idr, mblk,
		1L, 0, in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	spin_unlock(&dev->mblk_sl);
	idr_preload_end();
	if (ret < 0)
		goto err_idr;
	mblk->index = req.index = idx = ret;

	mblk->vaddr = dma_alloc_coherent(
		&dev->pcidev->dev, 
		req.size, 
		&mblk->paddr, 
		in_atomic() ? GFP_ATOMIC | GFP_KERNEL : GFP_KERNEL);
	if (!mblk->vaddr) {
		dev_err(&dev->dev, "Can't alloc DMA memory (size %u)", req.size);
		ret = -1;
		goto err_dma_alloc;
	}
	mblk->req_size = req.size;

	ret = cda_publish_mblk(mblk);
	if (ret) {
		dev_err(&dev->dev, "Can't publish mblk to sysfs: %d", ret);
		goto err_publish;
	}

	if(copy_to_user(ureq, &req, sizeof(req))) {
		ret = -EFAULT;
		goto err_copy_to_user;
	}

	spin_lock(&dev->mblk_sl);
	list_add(&mblk->list, &dev->mem_blocks);
	spin_unlock(&dev->mblk_sl);

	return 0;

err_copy_to_user:
	cda_hide_mblk(mblk);
err_publish:
	dma_free_coherent(&dev->pcidev->dev, mblk->req_size,
				mblk->vaddr, mblk->paddr);
err_dma_alloc:
	spin_lock(&dev->mblk_sl);
	idr_remove(&dev->mblk_idr, idx);
	spin_unlock(&dev->mblk_sl);
err_idr:
	kobject_put(&mblk->kobj);
out:
	return ret;
}

static void cda_free_mem(struct cda_mblk *mblk)
{
	cda_hide_mblk(mblk);
	dma_free_coherent(&mblk->dev->pcidev->dev, mblk->req_size,
		mblk->vaddr, mblk->paddr);
	kobject_put(&mblk->kobj);
}

int cda_free_mem_by_idx(struct cda_dev *dev, void *owner, void __user *ureq)
{
	int memidx;
	struct cda_mblk *mblk;
	if (copy_from_user(&memidx, (void __user *)ureq, sizeof(memidx))) {
		return -EFAULT;
	}

	spin_lock(&dev->mblk_sl);
	mblk = idr_find(&dev->mblk_idr, memidx);
	if (mblk && mblk->index == memidx) {
		if( mblk->owner != owner ) {
			dev_warn(&dev->dev, "Free mblk from another owner\n");
			idr_replace(&dev->mblk_idr, dev->dummy_blk, memidx);
		}
		list_del(&mblk->list);
	} else if(mblk) {
		dev_warn(&dev->dev, "Free mblk with index %d, required %d\n", mblk->index, memidx);
	}
	spin_unlock(&dev->mblk_sl);
	if (!mblk)
		return -ENOENT;
	if (mblk->index) {
		cda_free_mem(mblk);
		spin_lock(&dev->mblk_sl);
		idr_remove(&dev->mblk_idr, mblk->index);
		spin_unlock(&dev->mblk_sl);
	}
	return 0;
}

void cda_free_dev_mem(struct cda_dev *dev, void *owner)
{
	struct cda_mblk *mblk, *tmp;
	LIST_HEAD(mblks);

	spin_lock(&dev->mblk_sl);
	if( owner == NULL ){
		idr_destroy(&dev->mblk_idr);
		list_replace_init(&dev->mem_blocks, &mblks);
	} else {
		list_for_each_entry_safe(mblk, tmp, &dev->mem_blocks, list) {
			if( mblk->index > 0L && mblk->owner == owner ) {
				list_move(&mblk->list, &mblks);
				idr_replace(&dev->mblk_idr, dev->dummy_blk, mblk->index);
			}
		}
	}
	spin_unlock(&dev->mblk_sl);
	list_for_each_entry_safe(mblk, tmp, &mblks, list) {
		// Unmap blocks owned by specified owner or all if owner is NULL
		cda_free_mem(mblk);
		if( owner != NULL ){
			spin_lock(&dev->mblk_sl);
			idr_remove(&dev->mblk_idr, mblk->index);
			spin_unlock(&dev->mblk_sl);
		}
	}
}

static void cda_release_map(struct cda_mmap *memmap)
{	
	dma_unmap_sg(memmap->dev->pcidev == NULL ? NULL : &memmap->dev->pcidev->dev, memmap->sgt.sgl, memmap->sgt.orig_nents, DMA_BIDIRECTIONAL);
	unpin_user_pages_dirty_lock(memmap->pages, memmap->blk_cnt, 1);
	memmap->mapped_blk_cnt = 0;
}

static int cda_perform_mapping(
	struct cda_mmap *memmap)
{
	uint i;
	int nents;
	struct scatterlist *sg;
	ulong len = memmap->size;
	void __user *buf = memmap->vaddr;
	struct cda_drv_sg_item *cda_sg_list = memmap->sg_list;
	sg = memmap->sgt.sgl;
	for (i = 0; i < memmap->sgt.orig_nents; i++, sg = sg_next(sg)) {
		unsigned int offset = offset_in_page(buf);
		unsigned int nbytes =
			min_t(unsigned int, PAGE_SIZE - offset, len);

		sg_set_page(sg, memmap->pages[i], nbytes, offset);

		buf += nbytes;
		len -= nbytes;
	}

	nents = dma_map_sg(&memmap->dev->pcidev->dev, memmap->sgt.sgl, memmap->sgt.orig_nents, DMA_BIDIRECTIONAL);
	if (!nents) {
		dev_err(&memmap->dev->dev, "map sgl failed, sgt 0x%p.\n", &memmap->sgt);
		return -EIO;
	}
	memmap->sgt.nents = nents;

	for (i = 0, sg = memmap->sgt.sgl; i < nents; i++, sg = sg_next(sg)) {
		cda_sg_list[i].size = sg_dma_len(sg);
		cda_sg_list[i].paddr = sg_dma_address(sg);
	}

	memmap->mapped_blk_cnt = nents;
	return 0;
}

int cda_map_mem(struct cda_dev *dev, void *owner, void __user *ureq)
{
	int ret = -ENOMEM;
	int idx;
	int npages;
	struct cda_mmap *memmap;
	struct cda_map_mem req;
	unsigned long offset;
	void *req_vaddr;

	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;
	req_vaddr = (void __user *)req.vaddr;
	offset = offset_in_page(req_vaddr);
	npages = DIV_ROUND_UP(offset + req.size, PAGE_SIZE);
	memmap = kzalloc(sizeof(*memmap) + npages * (sizeof(struct cda_drv_sg_item) + sizeof(struct page *)), 
		in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	if (!memmap) {
		dev_err(&dev->dev, "Can't alloc memmap\n");
		goto out;
	}
	memmap->owner = owner;
	memmap->sg_list = (struct cda_drv_sg_item *)((void *)memmap + sizeof(*memmap));
	memmap->pages = (struct page **)((void *)memmap + sizeof(*memmap) + npages * (sizeof(struct cda_drv_sg_item)));

	if (sg_alloc_table(&memmap->sgt, npages, 
		in_atomic() ? GFP_ATOMIC :GFP_KERNEL)) {
		dev_err(&dev->dev, "Can't alloc sg table\n");
		goto out;
	}
	INIT_LIST_HEAD(&memmap->list);
	memmap->dev = dev;
	kobject_init(&memmap->kobj, &memmap_type);

	memmap->vaddr = req_vaddr;
	memmap->size = req.size;
	memmap->blk_cnt = npages;
	idr_preload(in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	spin_lock(&dev->mblk_sl);
	ret = idr_alloc(&dev->mblk_idr, memmap,
		1L, 0, in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	spin_unlock(&dev->mblk_sl);
	idr_preload_end();
	if (ret < 0)
		goto err_idr;
	memmap->index = req.index = idx = ret;
	
	ret = pin_user_pages_fast((ulong)req_vaddr, npages,
					FOLL_WRITE, memmap->pages);
	if ( ret < 0 ) {
		dev_err(&dev->pcidev->dev,
			"Pin user pages failed for addr=0x%p [ret=%d]\n",
			req_vaddr, ret);
		goto err_pin;
	}
	if (ret != npages) {
		dev_err(&dev->pcidev->dev,
			"Unable to pin all user pages for addr=0x%p\n", req_vaddr);
		ret = -EFAULT;
		goto err_pin;
	}

	ret = cda_perform_mapping(memmap);
	if ( ret ) {
		dev_err(&dev->dev, "Can't map user memory for DMA (size %u)", req.size);
		goto err_dma_alloc;
	}

	ret = cda_publish_memmap(memmap);
	if (ret) {
		dev_err(&dev->dev, "Can't publish memmap to sysfs: %d", ret);
		goto err_publish;
	}

	if(copy_to_user(ureq, &req, sizeof(req))) {
		ret = -EFAULT;
		goto err_copy_to_user;
	}

	spin_lock(&dev->mblk_sl);
	list_add(&memmap->list, &dev->mem_maps);
	spin_unlock(&dev->mblk_sl);

	dev_dbg(&dev->dev, "map vaddr %p, pages %d\n", memmap->vaddr, npages);
	return 0;

err_copy_to_user:
	cda_hide_memmap(memmap);
err_publish:
	cda_release_map(memmap);
err_dma_alloc:
	unpin_user_pages_dirty_lock(memmap->pages, memmap->blk_cnt, 1);	
err_pin:
	spin_lock(&dev->mblk_sl);
	idr_remove(&dev->mblk_idr, idx);
	spin_unlock(&dev->mblk_sl);
err_idr:
	kobject_put(&memmap->kobj);
out:
	if( memmap ) {
		if( memmap->pages ) {
			kfree(memmap->pages);
		}
		kfree(memmap);
	}
	return ret;
}

static void cda_free_map(struct cda_mmap *memmap)
{
	dev_dbg(&memmap->dev->dev, "unmap vaddr %p, pages %d\n", memmap->vaddr, memmap->blk_cnt);
	cda_hide_memmap(memmap);
	cda_release_map(memmap);
	kobject_put(&memmap->kobj);
}

int cda_unmap_mem_by_idx(struct cda_dev *dev, void *owner, void __user *ureq)
{
	int memidx;
	struct cda_mmap *memmap;
	if (copy_from_user(&memidx, (void __user *)ureq, sizeof(memidx)))
		return -EFAULT;

	spin_lock(&dev->mblk_sl);
	memmap = idr_find(&dev->mblk_idr, memidx);
	if (memmap && memmap->index == memidx) {
		if( memmap->owner != owner )
			dev_warn(&dev->dev, "Unmap buffer by another user\n");
		idr_replace(&dev->mblk_idr, dev->dummy_blk, memidx);
		list_del(&memmap->list);
	} else if (memmap)
		dev_warn(&dev->dev, "Unmap buffer with index %d, required %d\n", memmap->index, memidx);
	spin_unlock(&dev->mblk_sl);

	if (!memmap)
		return -ENOENT; // Somebody may already release this block in parallel

	if( memmap->index ) {
		cda_free_map(memmap);
		spin_lock(&dev->mblk_sl);
		idr_remove(&dev->mblk_idr, memmap->index);
		spin_unlock(&dev->mblk_sl);
	}
	return 0;
}

void cda_unmmap_dev_mem(struct cda_dev *dev, void *owner)
{
	struct cda_mmap *memmap, *tmp;
	LIST_HEAD(memmaps);

	spin_lock(&dev->mblk_sl);
	if( owner == NULL ){
		list_replace_init(&dev->mem_maps, &memmaps);
	} else {
		list_for_each_entry_safe(memmap, tmp, &dev->mem_maps, list) {
			if( memmap->index > 0L && memmap->owner == owner ) {
				idr_replace(&dev->mblk_idr, dev->dummy_blk, memmap->index);
				list_move(&memmap->list, &memmaps);
			}
		}
	}
	spin_unlock(&dev->mblk_sl);
	list_for_each_entry_safe(memmap, tmp, &memmaps, list) {
		// Unmap blocks owned by specified owner or all if owner is NULL
		cda_free_map(memmap);
		if( owner != NULL ){
			spin_lock(&dev->mblk_sl);
			idr_remove(&dev->mblk_idr, memmap->index);
			spin_unlock(&dev->mblk_sl);
		}
	}
}

int cda_mems_create(struct cda_dev *cdadev)
{
	int ret = sysfs_create_group(&cdadev->dev.kobj, &cda_attr_grp);
	if (ret)
		goto err_group;

	cdadev->kobj_mems = kobject_create_and_add("mems", &cdadev->dev.kobj);
	if (!cdadev->kobj_mems)
		goto err_mems;
	return 0;

err_mems:
	sysfs_remove_group(&cdadev->dev.kobj, &cda_attr_grp);
err_group:
	dev_err(&cdadev->dev, "Couldn't create sysfs files: %d\n", ret);
	return ret;
}

void cda_mems_release(struct cda_dev *dev)
{
	//cda_release_bars(dev);
	kobject_del(dev->kobj_mems);
	kobject_put(dev->kobj_mems);
	sysfs_remove_group(&dev->dev.kobj, &cda_attr_grp);
}
