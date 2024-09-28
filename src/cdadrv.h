// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2020 DeGirum Corp., Egor Pomozov.
//
// CDA linux driver mem blocks/mem maps and interrupt request handler
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/version.h>


#define CDA_MAX_DRV_SEMAPHORES (16)

struct cda_interrupts;
struct cda_bar;
struct cda_dev;
// Dummy block for fast releasing
struct cda_dummy_blk {
	struct cda_dev *dev;
	int index;
};

struct cda_dev {
    struct cdev cdev;
    struct device dev;

	int minor;
	struct list_head devices;

	struct pci_dev *pcidev;
	unsigned long stored_flags[PCI_ROM_RESOURCE];
	
	struct mutex ilock;
	struct cda_interrupts *ints;

	struct kobject *kobj_mems;
	struct cda_dummy_blk *dummy_blk;
	struct idr mblk_idr;
	spinlock_t mblk_sl;
	struct list_head mem_blocks;
	struct list_head mem_maps;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
	// Security kernel lock needs w/a to access BARs
	struct kobject *kobj_bars;
	struct cda_bar *sysfs_bar[PCI_ROM_RESOURCE]; // 6 BARs excl. ROM
#endif
	u64 semaphores[CDA_MAX_DRV_SEMAPHORES];
	void *sem_owner[CDA_MAX_DRV_SEMAPHORES];
};

int cda_alloc_mem(struct cda_dev *dev, void *owner, void __user *arg);
int cda_free_mem_by_idx(struct cda_dev *dev, void *owner, void __user *ureq);
int cda_map_mem(struct cda_dev *dev, void *owner, void __user *arg);
int cda_unmap_mem_by_idx(struct cda_dev *dev, void *owner, void __user *ureq);
void cda_unmmap_dev_mem(struct cda_dev *dev, void *owner);
void cda_free_dev_mem(struct cda_dev *dev, void *owner);
int cda_init_interrupts(struct cda_dev *dev, void *owner, void __user *ureq);
int cda_mems_create(struct cda_dev *dev);
int cda_free_irqs(struct cda_dev *dev, void *owner);
void cda_mems_release(struct cda_dev *dev);
int cda_req_int(struct cda_dev *dev, void *owner, void __user *ureq);
int cda_cancel_req(struct cda_dev *dev, void *owner);
int cda_open_bars(struct cda_dev *cdadev);
void cda_release_bars(struct cda_dev *cdadev);
int cda_sem_aq(struct cda_dev *cdadev, void *owner, void __user *ureq);
int cda_sem_rel(struct cda_dev *cdadev, void *owner, void __user *ureq);
void cda_sem_rel_by_owner(struct cda_dev *dev, void *owner);
