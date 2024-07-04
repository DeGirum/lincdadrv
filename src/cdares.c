// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2020 DeGirum Corp., Egor Pomozov.
//
// CDA linux driver mem blocks/mem maps and interrupt request handler
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

#include "cdadrv.h"
#include "cdaioctl.h"

struct cda_vector {
	volatile bool busy;
	wait_queue_head_t wait;
	atomic_t count;
	unsigned irq;
};

struct cda_interrupts {
	int num;
	enum int_type type;
	void *owner;
	struct cda_vector *vecs;
	struct msix_entry *msix_entries;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
struct cda_bar {
	struct kobject kobj;
	/* struct resource *res; */
	int index;
	phys_addr_t paddr;
	phys_addr_t len;
	void *vaddr;
	struct cda_dev *dev;
	struct bin_attribute mmap_attr;
};
#else
#if defined __has_attribute 
# if __has_attribute (__fallthrough__)
#  define fallthrough                    __attribute__((__fallthrough__))
# endif
#else
# define fallthrough                    do {} while (0)  /* fallthrough */
#endif //has_attribute
#endif // LINUX_VERSION_CODE

static int cda_alloc_msix(struct cda_dev *cdadev, uint32_t rvecs, struct cda_interrupts *ints)
{
	int i, ret;
	struct msix_entry *entries;
	entries = kcalloc(rvecs, sizeof(struct msix_entry), GFP_KERNEL);
	if( !entries ) {
		return -ENOMEM;
	}

	for (i = 0; i < rvecs; i++) {
		entries[i].entry = i;
		entries[i].vector = 0;
	}

	ret = pci_enable_msix_exact(cdadev->pcidev, entries, rvecs);
	if( !ret ) {
		ints->num = rvecs;
		ints->msix_entries = entries;
		ints->type = MSIX;
	} else {
		kfree(entries);
	}
	return ret;
}

static irqreturn_t cda_isr(int irq, void *priv)
{
	struct cda_vector *vec = priv;
	atomic_inc_return(&vec->count);
	wake_up(&vec->wait);
	return IRQ_HANDLED;
}

int cda_init_interrupts(struct cda_dev *cdadev, void *owner, void __user *ureq)
{
	int ret = 0;
	int nvecs, i;
	struct cda_vector *vec;
	struct cda_int_lock req;
	struct cda_interrupts *ints;

	if( cdadev->ints ) {
		dev_dbg(&cdadev->pcidev->dev, "Interrupts are already attached");
		return -EINVAL; // Already attached
	}
	if( copy_from_user(&req, (void __user *)ureq, sizeof(req)) )
		return -EFAULT;

	ints = kcalloc(1, sizeof(struct cda_interrupts), GFP_KERNEL);
	if( !ints )
		return -ENOMEM;

	ints->owner = owner;
	switch( req.inttype ) {
	case MSIX:
		ret = cda_alloc_msix(cdadev, req.vectors, ints);
		if( !ret ) {
			nvecs = req.vectors;
			break;
		}
		if( ret == -ENOMEM ) {
			kfree(ints);
			return ret;
		}
		dev_warn(&cdadev->pcidev->dev, "No MSI-X vectors, try MSI. Error %x\n", ret);
		fallthrough;
	case MSI:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
		nvecs = pci_alloc_irq_vectors(cdadev->pcidev, 1, req.vectors, PCI_IRQ_MSI);
#else
		nvecs = pci_alloc_irq_vectors_affinity(cdadev->pcidev, 1, req.vectors, PCI_IRQ_MSI, NULL);
#endif
		if( nvecs > 0 ) {
			ints->num = nvecs;
			ints->type = MSI;
			break;
		}
		dev_warn(&cdadev->pcidev->dev, "No MSI vectors, try legacy. Error %x\n", nvecs);
		fallthrough;
	case LEGACY_INTERRUPT:
		ints->num = 1;
		ints->type = LEGACY_INTERRUPT;
		break;
	}

	ints->vecs = kcalloc(ints->num, sizeof(struct cda_vector), GFP_KERNEL);
	if( !ints->vecs ) {
		ret = -ENOMEM;
		goto err_alloc_vecs;
	}

	for( i = 0; i < ints->num; i++ ) {
		char name[10];
		vec = &ints->vecs[i];
		vec->irq = ints->type == MSIX ? 
					cdadev->ints->msix_entries[i].vector : 
					cdadev->pcidev->irq + i;
		snprintf(name, sizeof(name), "cda%02d-%x", cdadev->minor, i);
		init_waitqueue_head(&vec->wait);
		atomic_set(&vec->count, 0);
		ret = request_irq(vec->irq, cda_isr, ints->type == LEGACY_INTERRUPT ? IRQF_SHARED : 0, name, vec);
		if( ret ) {
			dev_err(&cdadev->pcidev->dev, "request_irq failed for vector %d: %d", i, ret);
			break;
		}
	}

	// Return interrupt type and vector count to user
	if( !ret ) {
		req.inttype = ints->type;
		req.vectors = ints->num;
		if( copy_to_user(ureq, &req, sizeof(req)) )
			ret = -EFAULT;
	}

	if( !ret ) {
		cdadev->ints = ints;
		return ret;
	}
	// Fail. Release
	for( i -= 1; i >= 0; i-- ) {
		struct cda_vector *vec = &ints->vecs[i];
		free_irq(vec->irq, vec);
	}
	kfree(ints->vecs);

err_alloc_vecs:
	pci_free_irq_vectors(cdadev->pcidev);
	kfree(ints->msix_entries);
	kfree(ints);

	return ret;
}

int cda_free_irqs(struct cda_dev *cdadev, void *owner)
{
	int i;
	struct cda_interrupts *ints;
	if( cdadev->ints == NULL )
		return -EINVAL;
	if( cdadev->ints->owner != owner ) {
		dev_dbg(&cdadev->pcidev->dev, "Interrupts are not owned by %p", owner);
		return -EINVAL;
	}
	mutex_lock(&cdadev->ilock);
	ints = cdadev->ints;
	cdadev->ints = NULL;
	mutex_unlock(&cdadev->ilock);
	if( ints && ints->num > 0 ) {
		for( i = 0; i < ints->num; i++ ) {
			struct cda_vector *vec = &ints->vecs[i];
			while( vec->busy ) {
				wake_up(&vec->wait);
				udelay(1);
			}
			free_irq(vec->irq, vec);
		}
		pci_free_irq_vectors(cdadev->pcidev);
		kfree(ints->vecs);
		kfree(ints->msix_entries);
		kfree(ints);
	}
	return 0;
}

int cda_req_int(struct cda_dev *cdadev, void *owner, void __user *ureq)
{
	struct cda_interrupts *ints;
	struct cda_req_int req;
	struct cda_vector *vec;
	unsigned long timeout;
	unsigned count;

	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;

	if( cdadev->ints == NULL )
		return -EINVAL;

	if( cdadev->ints->owner != owner ) {
		dev_err(&cdadev->pcidev->dev, "Interrupts are not owned by %p", owner);
		return -EINVAL;
	}

	mutex_lock(&cdadev->ilock);
	ints = cdadev->ints;
	if( !ints || (req.vector > ints->num) ) {
		mutex_unlock(&cdadev->ilock);
		return -EINVAL;
	}

	vec = &ints->vecs[req.vector];
	if (req.reset)
		atomic_set(&vec->count, 0);

	timeout = nsecs_to_jiffies(req.timeout);
	count = atomic_xchg(&vec->count, 0);
	if( !count )
	{
		vec->busy = true;
		mutex_unlock(&cdadev->ilock);
		timeout = wait_event_interruptible_timeout(vec->wait,
							(count = atomic_xchg(&vec->count, 0)),
							timeout);
		mutex_lock(&cdadev->ilock);
		vec->busy = false;
	}
	mutex_unlock(&cdadev->ilock);
	dev_dbg(&cdadev->pcidev->dev, "Interrupt vector %d timeout: %ld count %u reset %d\n", req.vector, timeout, count, req.reset);
	return timeout > 0 ? 0 : timeout == 0 ? -ETIME : timeout;
}

int cda_cancel_req(struct cda_dev *cdadev, void *owner)
{
	int i;
	struct cda_interrupts *ints;
	if( cdadev->ints == NULL )
		return -EINVAL;

	if( cdadev->ints->owner != owner ) {
		dev_dbg(&cdadev->pcidev->dev, "Interrupts are not owned by %p", owner);
		return -EINVAL;
	}

	mutex_lock(&cdadev->ilock);
	ints = cdadev->ints;
	for (i = 0; ints && i < ints->num; i++) {
		if( ints->vecs[i].busy )
			wake_up(&ints->vecs[i].wait);
	}
	mutex_unlock(&cdadev->ilock);
	return 0;
}

int cda_sem_aq(struct cda_dev *cdadev, void *owner, void __user *ureq)
{
	int res = 0;
	struct cda_sem_aq req;
	u64 cur_time;
	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;

	mutex_lock(&cdadev->ilock);
	cur_time = ktime_get_ns();
	if( cdadev->semaphores[req.sem_id] < cur_time ) {
		cdadev->semaphores[req.sem_id] = cur_time + req.time_ns > cur_time ? cur_time + req.time_ns : 0xFFFFFFFFFFFFFFFFULL;
		cdadev->sem_owner[req.sem_id] = owner;
	} else {
		res = 1;
	}
	mutex_unlock(&cdadev->ilock);
	return res;
}

int cda_sem_rel(struct cda_dev *cdadev, void *owner, void __user *ureq)
{
	int res = 0;
	int req_sem;
	if (copy_from_user(&req_sem, ureq, sizeof(req_sem)))
		return -EFAULT;
	if( cdadev->sem_owner[req_sem] != owner ) {
		dev_warn(&cdadev->pcidev->dev, "Semaphore %d is not owned by %p", req_sem, owner);
	} else {
		mutex_lock(&cdadev->ilock);
		cdadev->semaphores[req_sem] = 0ULL;
		cdadev->sem_owner[req_sem] = NULL;
		mutex_unlock(&cdadev->ilock);
	}
	return res;
}

void cda_sem_rel_by_owner(struct cda_dev *dev, void *owner)
{
	uint32_t i;
	mutex_lock(&dev->ilock);
	for( i = 0; i < CDA_MAX_DRV_SEMAPHORES; i++ ) {
		if( dev->sem_owner[i] == owner ) {
			dev->semaphores[i] = 0ULL;
			dev->sem_owner[i] = NULL;
		}
	}
	mutex_unlock(&dev->ilock);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
#define to_bar(obj) container_of((obj), struct cda_bar, kobj)
struct bar_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct cda_bar *, char *);
	ssize_t (*store)(struct cda_bar *, char*, size_t);
};

#define cdadev_bar_attr(_field, _fmt)					\
	static ssize_t							\
	bar_##_field##_show(struct cda_bar *bar, char *buf)		\
	{								\
		return sprintf(buf, _fmt, bar->_field);			\
	}								\
	static struct bar_sysfs_entry bar_##_field##_attr =		\
		__ATTR(_field, S_IRUGO, bar_##_field##_show, NULL);

#pragma GCC diagnostic ignored "-Wformat"
cdadev_bar_attr(paddr, "0x%lx\n");
cdadev_bar_attr(len, "0x%lx\n");
cdadev_bar_attr(index, "%d\n");
#pragma GCC diagnostic warning "-Wformat"

static ssize_t bar_attr_show(struct kobject *kobj, struct attribute *attr,
				 char *buf)
{
	struct cda_bar *bar = to_bar(kobj);
	struct bar_sysfs_entry *entry =
		container_of(attr, struct bar_sysfs_entry, attr);

	if (!entry->show)
		return -EIO;

	return entry->show(bar, buf);
}

static const struct sysfs_ops bar_ops = {
	.show = bar_attr_show,
};

static void bar_release(struct kobject *kobj)
{
	struct cda_bar *bar = to_bar(kobj);
	kfree(bar);
}

static struct attribute *bar_attrs[] = {
	&bar_paddr_attr.attr,
	&bar_len_attr.attr,
	&bar_index_attr.attr,
	NULL,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
ATTRIBUTE_GROUPS(bar);
#endif

static struct kobj_type bar_type = {
	.sysfs_ops = &bar_ops,
	.release = bar_release,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
	.default_groups = bar_groups,
#else
	.default_attrs = bar_attrs,
#endif
};

// Secure enable support
static const struct vm_operations_struct pci_phys_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

static int bar_mmap( struct file *file, 
						struct kobject *kobj, 
						struct bin_attribute *attr,
			   			struct vm_area_struct *vma)
{
	struct cda_bar *bar = attr->private;
	unsigned long requested = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	unsigned long pages = (unsigned long)bar->len >> PAGE_SHIFT;
	unsigned long size;

	if (vma->vm_pgoff + requested > pages)
		return -EINVAL;

	size = ((pci_resource_len(bar->dev->pcidev, bar->index) - 1) >> PAGE_SHIFT) + 1;
	if (vma->vm_pgoff + vma_pages(vma) > size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
	vma->vm_pgoff += (pci_resource_start(bar->dev->pcidev, bar->index) >> PAGE_SHIFT);
	vma->vm_ops = &pci_phys_vm_ops;

	return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
									vma->vm_end - vma->vm_start,
									vma->vm_page_prot);
}

int cda_open_bars(struct cda_dev *cdadev)
{
	int i;
	int ret;
	struct cda_bar *bar;
	struct resource *res_child;
	int bars = pci_select_bars(cdadev->pcidev, IORESOURCE_MEM);

	ret = -EINVAL;
	cdadev->kobj_bars = kobject_create_and_add("bars", &cdadev->dev.kobj);
	if (!cdadev->kobj_bars)
		goto err;

	for (i = 0; bars && i < PCI_ROM_RESOURCE; bars >>= 1, i++){
		struct bin_attribute *mmap_attr;
		if (!(bars & 1))
			continue;

		if( !(pci_resource_flags(cdadev->pcidev, i) & IORESOURCE_MEM) )
			continue;
		
		ret = -ENOMEM;
		bar = kzalloc(sizeof(*bar), GFP_KERNEL);
		if (!bar)
			goto err;
		bar->index = i;
		bar->paddr = pci_resource_start(cdadev->pcidev, i);
		bar->len = pci_resource_len(cdadev->pcidev, i);
		bar->vaddr = NULL;
		bar->dev = cdadev;
		cdadev->sysfs_bar[i] = bar;
		kobject_init(&bar->kobj, &bar_type);

		if( (bar->vaddr = pci_iomap(cdadev->pcidev, i, bar->len)) == NULL )
			goto err;
		ret = kobject_add(&bar->kobj, cdadev->kobj_bars, "mmio_bar%d", i);
		if (ret)
			goto err;

		mmap_attr = &bar->mmap_attr;
		mmap_attr->mmap = bar_mmap;
		mmap_attr->attr.name = "mmap";
		mmap_attr->attr.mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
		mmap_attr->size = bar->len;
		mmap_attr->private = bar;

		ret = sysfs_create_bin_file(&bar->kobj, mmap_attr);
		if (ret) {
			dev_err(&cdadev->dev, "Can't create kobject file for mmap");
			goto err;
		}

		//Drop busy bit
		res_child = cdadev->pcidev->resource[i].child;

		dev_info(&cdadev->dev, "Store resource %d flag: 0x%lx\n", i, res_child->flags);
		cdadev->stored_flags[i] = res_child->flags;
		if (IORESOURCE_BUSY & res_child->flags) {
			res_child->flags &= ~IORESOURCE_BUSY;
		}
	}
	return 0;

err:
	cda_release_bars(cdadev);
	return ret;
}

void cda_release_bars(struct cda_dev *cdadev)
{
	int i;
	int bars = pci_select_bars(cdadev->pcidev, IORESOURCE_MEM);
	for( i = 0; i < PCI_ROM_RESOURCE; i++ ) {
		struct cda_bar *bar = cdadev->sysfs_bar[i];
		if (!bar)
			continue;

		cdadev->sysfs_bar[i] = NULL;
		sysfs_remove_bin_file(&bar->kobj, &bar->mmap_attr);
		kobject_del(&bar->kobj);
		kobject_put(&bar->kobj);

		if( bars & (1 << i) ) {
			cdadev->pcidev->resource[i].child->flags = cdadev->stored_flags[i];
			dev_info(&cdadev->dev, "Restore resource %d flag: %lx\n", i, cdadev->stored_flags[i]);
		}
	}

	kobject_del(cdadev->kobj_bars);
	kobject_put(cdadev->kobj_bars);
}
#else // LINUX_VERSION_CODE < KERNEL_VERSION(5,4,0)
int cda_open_bars(struct cda_dev *cdadev)
{
	int i;
	struct resource *res_child;
	int bars = pci_select_bars(cdadev->pcidev, IORESOURCE_MEM);

	for( i = 0; i < PCI_ROM_RESOURCE; i++ ) {
		// Drop busy bit
		if( bars & (1 << i) ) {
			res_child = cdadev->pcidev->resource[i].child;
			cdadev->stored_flags[i] = res_child->flags;
			dev_info(&cdadev->dev, "Store resource %d flag: 0x%lx\n", i, res_child->flags);
			if( IORESOURCE_BUSY & res_child->flags ) {
				res_child->flags &= ~IORESOURCE_BUSY;
				dev_dbg(&cdadev->dev, "Drop busy bit for resource %d", i);
			}
		}
	}
	return 0;
}

void cda_release_bars(struct cda_dev *cdadev)
{
	int i;
	int bars = pci_select_bars(cdadev->pcidev, IORESOURCE_MEM);
	for( i = 0; i < PCI_ROM_RESOURCE; i++ ) {
		if( bars & (1 << i) ) {
			cdadev->pcidev->resource[i].child->flags = cdadev->stored_flags[i];
			dev_info(&cdadev->dev, "Restore resource %d flag: %lx\n", i, cdadev->stored_flags[i]);
		}
	}
}
#endif
