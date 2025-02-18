/* SPDX-License-Identifier: LGPL-2.1 */
// Copyright(c) 2020 DeGirum Corp., Egor Pomozov.
//
// CDA linux driver mem blocks/mem maps and interrupt request handler
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the Lesser GNU General Public License,
// version 3, as published by the Free Software Foundation.
//
#ifndef LINUX_VERSION_CODE
#include <stdint.h> // only include if we're not compiling against the kernel
#endif

#define CDA_IOCTL_MAGIC 'C'
#define CDA_ALLOC_MEM _IOWR(CDA_IOCTL_MAGIC, 0x1, long)
#define CDA_FREE_MEM _IOW(CDA_IOCTL_MAGIC, 0x2, long)
#define CDA_MAP_MEM _IOWR(CDA_IOCTL_MAGIC, 0x3, long)
#define CDA_UNMAP_MEM _IOW(CDA_IOCTL_MAGIC, 0x4, long)
#define CDA_INIT_INT _IOWR(CDA_IOCTL_MAGIC, 0x5, long)
#define CDA_FREE_INT _IOW(CDA_IOCTL_MAGIC, 0x6, long)
#define CDA_REQ_INT _IOWR(CDA_IOCTL_MAGIC, 0x7, long)
#define CDA_INT_CANCEL _IOWR(CDA_IOCTL_MAGIC, 0x8, long)
#define CDA_SEM_AQ _IOW(CDA_IOCTL_MAGIC, 0x9, long)
#define CDA_SEM_REL _IOW(CDA_IOCTL_MAGIC, 0xA, long)

struct cda_alloc_mem {
	uint32_t size;
	uint32_t index;
};

struct cda_map_mem {
	uintptr_t vaddr;
	uint32_t size;
	uint32_t index;
};

struct cda_drv_sg_item {
	uint64_t paddr;
	uint32_t size;
};

struct cda_req_int {
	uint32_t vector;
	uint64_t timeout;
	uint32_t reset;
};

enum int_type {
	LEGACY_INTERRUPT = 0,
	MSI = 1,
	MSIX = 2
};

struct cda_int_lock {
	uint32_t inttype;
	uint32_t vectors;
};

struct cda_sem_aq {
	uint32_t sem_id;
	uint64_t time_ns;
};

// Limitation: This mapping affects the Number of mblk instances to max of 256 per device.

#define MBLK_IDX_SIG           7
#define BAR_IDX_SIG            8
#define SIG_BPOS               24
#define IDX_BPOS               20
#define BAR_OFFSET_MAPPING     (BAR_IDX_SIG << SIG_BPOS)
#define MBLK_OFFSET_MAPPING    (MBLK_IDX_SIG << SIG_BPOS)
#define USR_BAR_OFF(idx)       ((((idx)&0x7) << IDX_BPOS) | BAR_OFFSET_MAPPING)
#define USR_MBLK_OFF(idx)      ((((idx)&0xFF) << IDX_BPOS) | MBLK_OFFSET_MAPPING)
#define BAR_ID_OFF_VMA(off)    (((((off) << PAGE_SHIFT) & ~BAR_OFFSET_MAPPING) >> IDX_BPOS)&0x7)
#define MBLK_ID_OFF_VMA(off)   (((((off) << PAGE_SHIFT) & ~MBLK_OFFSET_MAPPING) >> IDX_BPOS)&0xFF)
#define IS_BAR_MAPPING(off)    (((off) >> (SIG_BPOS - PAGE_SHIFT)) == BAR_IDX_SIG)
#define IS_MBLK_MAPPING(off)   (((off) >> (SIG_BPOS - PAGE_SHIFT)) == MBLK_IDX_SIG)
