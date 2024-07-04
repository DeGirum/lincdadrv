// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2020 DeGirum Corp., Egor Pomozov.
//
// CDA linux driver mem blocks/mem maps and interrupt request handler
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/fs.h>

#include "cdadrv.h"
#include "cdaioctl.h"

MODULE_AUTHOR("DeGirum Corp., Egor Pomozov");
MODULE_DESCRIPTION("CDA linux driver to access pci devices");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.5.0.2");
// The version has to be in the format n.n.n.n, where each n is a single digit

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,9,0)
#error Too old kernel
#endif

static dev_t cdadev_first;
static const char cda_name[] = "cda";
static int req_pci_did = 0;
static int req_pci_vid = 0;
static int test_probe = 0;

#define CDA_DEV_MINOR_MAX 32
static DEFINE_SPINLOCK(cdadevlist_sl);
static DEFINE_IDA(cdaminor_ida);
static LIST_HEAD(cdadevs);

// Module parameters
module_param_named(did, req_pci_did, int, 0644);
MODULE_PARM_DESC(did, "Set required PCI device ID");
module_param_named(vid, req_pci_vid, int, 0644);
MODULE_PARM_DESC(vid, "Set required PCI vendor ID");
module_param_named(test_probe, test_probe, int, 0644);
MODULE_PARM_DESC(test_probe, "Check permissions to load driver");

static void cdadev_release(struct device *kdev);
static void cda_pci_remove(struct pci_dev *pcidev);
static int cda_pci_probe(struct pci_dev *pcidev,
			       const struct pci_device_id *id);

static int cda_cdev_open(struct inode *ino, struct file *file);
static int cda_cdev_release(struct inode *ino, struct file *file);
static long cda_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static struct pci_device_id cda_pci_ids[] = {
	{ PCI_DEVICE(0x1f0d, 0x0100) },
	{ PCI_DEVICE(0x1f0d, 0x8101) },
	{ PCI_DEVICE(0x1f0d, 0x0101) },
	{ PCI_DEVICE(0x10ee, 0x8011) },
	{ PCI_DEVICE(0, 0) }, 
	{ PCI_DEVICE(0, 0) },
};

static struct pci_driver cda_pci = {
    .name = cda_name,
    .probe = cda_pci_probe,
    .remove = cda_pci_remove,
    .id_table = cda_pci_ids,
};

static struct file_operations cda_fileops = {
	.owner = THIS_MODULE,
	.open = cda_cdev_open,
	.release = cda_cdev_release,
    .unlocked_ioctl = cda_cdev_ioctl,

};

static struct class cda_class = {
	.name = cda_name,
	.dev_release = cdadev_release,
};
/*
static inline bool cda_kernel_is_locked_down(void)
{
#ifdef CONFIG_LOCK_DOWN_KERNEL
#ifdef CONFIG_LOCK_DOWN_IN_EFI_SECURE_BOOT / * fedora * /
	return kernel_is_locked_down(NULL);
#elif CONFIG_EFI_SECURE_BOOT_LOCK_DOWN / * ubuntu * /
	return kernel_is_locked_down();
#else
	return false;
#endif
#else
	return false;
#endif
}
*/
static void cdadev_free(struct cda_dev *cdadev)
{
	ida_simple_remove(&cdaminor_ida, cdadev->minor);
	device_del(&cdadev->dev);
	put_device(&cdadev->dev);
}

static int cdadev_init(struct cda_dev *cdadev)
{
    // Create and initialize device structures
	int ret = -ENOMEM;
	struct device *dev = &cdadev->dev;
	device_initialize(dev);

	dev->class = &cda_class;
	dev->parent = &cdadev->pcidev->dev;
	
	cdadev->dummy_blk = kzalloc(sizeof(*cdadev->dummy_blk), in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	if (!cdadev->dummy_blk) {
		dev_err(&cdadev->pcidev->dev, "Can't alloc dummy blk\n");
		goto alloc_dummy;
	}
	idr_init(&cdadev->mblk_idr);
	ret = ida_simple_get(&cdaminor_ida, 0, CDA_DEV_MINOR_MAX, GFP_KERNEL);
	if( ret < 0 )
		goto err_minor_get;

	cdadev->minor = ret;
	dev->devt = MKDEV(MAJOR(cdadev_first), cdadev->minor);
	ret = dev_set_name(dev, "cda%02d", cdadev->minor);
	if (ret)
		goto err_set_name;

	ret = device_add(dev);
	if (ret) {
		dev_err(&cdadev->pcidev->dev, "Unable to create device. Error 0x%x\n", ret);
		goto err_device_add;
	}

	INIT_LIST_HEAD(&cdadev->mem_maps);
	INIT_LIST_HEAD(&cdadev->mem_blocks);
	spin_lock_init(&cdadev->mblk_sl);

	mutex_init(&cdadev->ilock);
	cdadev->ints = NULL;

	return 0;
err_device_add:
err_set_name:
	ida_simple_remove(&cdaminor_ida, cdadev->minor);
err_minor_get:
alloc_dummy:
	put_device(dev);
	return ret;
}

static int cda_pci_init(struct pci_dev *pcidev)
{
    // PCI initialization
	int ret;
	ret = pci_enable_device_mem(pcidev);
	if( ret ) {
		dev_err(&pcidev->dev, "Cannot enable PCI device mem\n");
		goto err_en_devmem;
	}

	if( dma_set_mask_and_coherent(&pcidev->dev, DMA_BIT_MASK(64)) &&
		(ret = dma_set_mask_and_coherent(&pcidev->dev, DMA_BIT_MASK(32))) ) {
		dev_err(&pcidev->dev, "Set DMA mask 32/64 failed: 0x%x\n", ret);
		goto err_dma_set_mask;
	}

	ret = pci_request_regions(pcidev, cda_name);
	if( ret ) {
		dev_err(&pcidev->dev, "Fail request regions: 0x%x\n", ret);
		goto err_req_regions;
	}

	pci_set_master(pcidev);
	return 0;
err_req_regions:
err_dma_set_mask:
	pci_disable_device(pcidev);
err_en_devmem:
	return ret;
}

static int cda_cdev_init(struct cda_dev *cdadev)
{
	int ret;
	struct cdev *cdev = &cdadev->cdev;

	cdev_init(cdev, &cda_fileops);
	cdev->owner = THIS_MODULE;
	kobject_set_name(&cdev->kobj, "%s%d", cda_name, cdadev->minor);
	ret = cdev_add(cdev, MKDEV(MAJOR(cdadev_first), cdadev->minor), CDA_DEV_MINOR_MAX);
	if (ret)
		return ret;
    return 0;
}

static int cda_pci_probe(struct pci_dev *pcidev, 
                        const struct pci_device_id *id)
{
	int ret;
	struct cda_dev *cdadev = kzalloc(sizeof(*cdadev), in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	if (!cdadev) {
		return -ENOMEM;
	}

	cdadev->pcidev = pcidev;
	ret = cdadev_init(cdadev);
	if( ret ) 
		goto err_cdadev_init;

	ret = cda_pci_init(pcidev);
	if( ret ) 
		goto err_pci_init;

	ret = cda_mems_create(cdadev);
	if( ret )
		goto err_sysfsmem;

	ret = cda_open_bars(cdadev);
	if( ret )
		goto err_check_bar;

	ret = cda_cdev_init(cdadev);
	if( ret )
		goto err_cdev_init;

	spin_lock(&cdadevlist_sl);
	list_add(&cdadev->devices, &cdadevs);
	spin_unlock(&cdadevlist_sl);

	pci_set_drvdata(pcidev, cdadev);
	return 0;
err_cdev_init:
	cda_release_bars(cdadev);
err_check_bar:
	cda_mems_release(cdadev);
err_sysfsmem:
	pci_release_regions(pcidev);
	pci_disable_device(pcidev);
err_pci_init:
	cdadev_free(cdadev);
err_cdadev_init:
	return ret;
}

static void cda_pci_remove(struct pci_dev *pcidev)
{
	struct cda_dev *cdadev = pci_get_drvdata(pcidev);

	if (!cdadev)
		return;

	spin_lock(&cdadevlist_sl);
	list_del(&cdadev->devices);
	spin_unlock(&cdadevlist_sl);

	cdev_del(&cdadev->cdev);
	cda_release_bars(cdadev);

	cda_mems_release(cdadev);

	cda_free_irqs(cdadev, NULL);
    cda_unmmap_dev_mem(cdadev, NULL);
    cda_free_dev_mem(cdadev, NULL);

	pci_release_regions(pcidev);
	pci_disable_device(pcidev);

	cdadev_free(cdadev);
}

static int cda_cdev_open(struct inode *ino, struct file *file)
{
	int ret;
	struct cda_dev *cdadev = 
		container_of(ino->i_cdev,
		struct cda_dev,
		cdev);
	if (!cdadev) {
		ret = -ENODEV;
		goto out;
	}
	get_device(&cdadev->dev);
	file->private_data = cdadev;
	return nonseekable_open(ino, file);
out:
	return ret;
}

static int cda_cdev_release(struct inode *ino, struct file *file)
{
	struct cda_dev *cdadev = file->private_data;
	if (!cdadev)
		return -ENODEV;

	cda_cancel_req(cdadev, (void *)file);
	cda_free_irqs(cdadev, (void *)file);
    cda_unmmap_dev_mem(cdadev, (void *)file);
    cda_free_dev_mem(cdadev, (void *)file);
    cda_sem_rel_by_owner(cdadev, (void *)file);

	put_device(&cdadev->dev);
	return 0;
}

static long cda_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cda_dev *cdadev = file->private_data;
	switch (cmd) {

	case CDA_ALLOC_MEM:
		return cda_alloc_mem(cdadev, (void *)file, (void __user *)arg);

	case CDA_FREE_MEM:
		return cda_free_mem_by_idx(cdadev, (void *)file, (void __user *)arg);

	case CDA_MAP_MEM:
		return cda_map_mem(cdadev, (void *)file, (void __user *)arg);

	case CDA_UNMAP_MEM:
		return cda_unmap_mem_by_idx(cdadev, (void *)file, (void __user *)arg);

	case CDA_INIT_INT:
		return cda_init_interrupts(cdadev, (void *)file, (void __user *)arg);

	case CDA_FREE_INT:
		return cda_free_irqs(cdadev, (void *)file);

	case CDA_REQ_INT: 
		return cda_req_int(cdadev, (void *)file, (void __user *) arg);

	case CDA_INT_CANCEL:
		return cda_cancel_req(cdadev, (void *)file);

	case CDA_SEM_AQ: 
		return cda_sem_aq(cdadev, (void *)file, (void __user *) arg);

	case CDA_SEM_REL:
		return cda_sem_rel(cdadev, (void *)file, (void __user *) arg);

	default:
		return -ENOTTY;
	}
}

static void cdadev_release(struct device *dev)
{
	struct cda_dev *cdadev = container_of(dev, struct cda_dev, dev);
	kfree(cdadev);
}

static int __init cdadrv_init(void)
{
    int ret;
	size_t pci_id_table_size = ARRAY_SIZE(cda_pci_ids);
	if( test_probe ) {
		printk("Test run. Nothing initialized\n");
		return 0;
	}

	ret = alloc_chrdev_region(&cdadev_first, 0, CDA_DEV_MINOR_MAX, cda_name);
	if( ret )
		goto err_alloc_cdev_reg;

	ret = class_register(&cda_class);
	if( ret )
		goto err_cls_reg;

	if( (req_pci_did || req_pci_vid) && pci_id_table_size >= 2 ) {
		// Last table element is 0,0
		// Update pre-last item
		cda_pci_ids[pci_id_table_size-2].vendor = req_pci_vid;
		cda_pci_ids[pci_id_table_size-2].device = req_pci_did;
	}
	ret = pci_register_driver(&cda_pci);
	if( ret )
        goto err_pci_reg_drv;

	return 0;

err_pci_reg_drv:
	class_unregister(&cda_class);
err_cls_reg:
	unregister_chrdev_region(cdadev_first, CDA_DEV_MINOR_MAX);
err_alloc_cdev_reg:
	return ret;
}

static void __exit dcadrv_exit(void)
{	
	if( test_probe ) { 
		printk("Stop test run. Nothing initialized\n");
		return;
	}
    pci_unregister_driver(&cda_pci);
	class_unregister(&cda_class);
	unregister_chrdev_region(cdadev_first, CDA_DEV_MINOR_MAX);
}

module_init(cdadrv_init);
module_exit(dcadrv_exit);
