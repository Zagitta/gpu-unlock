/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_GFW_EVAL_H
#define GPU_UNLOCK_GFW_EVAL_H

#include "registers.h"

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t u16;
typedef uint32_t u32;
#endif

/*
 * Offsets and fields from the pinned 610.43.03 GA102 headers:
 * dev_gc6_island.h, dev_gc6_island_addendum.h, dev_gsp.h, dev_falcon_v4.h.
 * GFW completion uses kflcnWaitForHalt_TU102 (Falcon CPUCTL), not RISC-V CPUCTL.
 */
#define GU_GFW_TIMEOUT_US 2050000u
#define GU_GFW_PLM_LEVEL0 0x1u
#define GU_GFW_BOOT_COMPLETED 0xffu
#define GU_GFW_CPUCTL_HALTED 0x10u
#define GU_PCI_COMMAND_IO 0x1u
#define GU_PCI_COMMAND_MEMORY 0x2u
#define GU_PCI_COMMAND_MASTER 0x4u
#define GU_PCI_PM_CTRL_STATE_MASK 0x3u
#define GU_PCI_PM_STATE_D0 0x0u

struct gpu_unlock_gfw_sample {
	bool config_ok;
	u32 plm;
	u32 boot;
	u32 cpuctl;
};

/* 0 complete, -EAGAIN still running, -ENODEV inaccessible. */
int gpu_unlock_gfw_assess(const struct gpu_unlock_gfw_sample *sample);

/* Put memory and bus-master bits back. Leave every other command bit alone. */
u16 gpu_unlock_command_restore(u16 original, u16 live);

/* PMCSR reports D0. PCI_D0 is the kernel's pci_power_t, not a PMCSR field. */
bool gpu_unlock_pmcsr_d0(u16 pmcsr);

#endif
