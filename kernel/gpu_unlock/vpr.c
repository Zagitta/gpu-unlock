// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/io.h>

#include "host.h"
#include "memory.h"
#include "recipes.h"
#include "registers.h"
#include "vpr.h"

static int vpr_read(void __iomem *bar, resource_size_t len, u32 reg, u32 *value)
{
	if (!bar || (u64)reg + 4 > len)
		return -ERANGE;
	*value = readl(bar + reg);
	if (*value == ~0u || (*value & 0xffff0000u) == 0xbadf0000u)
		return -EIO;
	return 0;
}

static int cold_boot(void __iomem *bar, resource_size_t len)
{
	u32 interrupt, handoff;
	int ret;

	ret = vpr_read(bar, len, GU_GC6_INTERRUPT_OFFSET, &interrupt);
	if (!ret)
		ret = vpr_read(bar, len, GU_GC6_HANDOFF_OFFSET, &handoff);
	if (ret)
		return ret;
	return (interrupt & GU_GC6_EXIT_PENDING) || (handoff & 1) ? -EBUSY : 0;
}

int gu_vpr_prepare(void __iomem *bar, resource_size_t len, struct gu_vpr_boot *boot)
{
	struct gu_memory_info memory;
	u32 size, sync, lock_low, lock_high, range, display;
	u64 start, end, saved_start, saved_end, tail;
	int ret;

	*boot = (struct gu_vpr_boot) { 0 };
#define GU_VPR_READ(reg, value) do { ret = vpr_read(bar, len, (reg), &(value)); \
	if (ret) return ret; } while (0)
	GU_VPR_READ(GU_VPR_CONTROL_OFFSET, boot->control);
	if (!(boot->control & GU_VPR_IN_USE))
		return 0;
	ret = cold_boot(bar, len);
	if (ret)
		return ret;
	GU_VPR_READ(GU_VPR_ADDR_LO_OFFSET, boot->low);
	GU_VPR_READ(GU_VPR_ADDR_HI_OFFSET, boot->high);
	GU_VPR_READ(GU_VPR_BOOT_HANDOFF_OFFSET, boot->handoff);
	GU_VPR_READ(GU_VPR_BOOT_SIZE_OFFSET, size);
	GU_VPR_READ(GU_VPR_SYNC_SCRUB_MIB_OFFSET, sync);
	GU_VPR_READ(GU_MMU_LOCK_ADDR_LO_OFFSET, lock_low);
	GU_VPR_READ(GU_MMU_LOCK_ADDR_HI_OFFSET, lock_high);
	GU_VPR_READ(GU_LOCAL_MEMORY_RANGE_OFFSET, range);
	GU_VPR_READ(GU_DISPLAY_FUSE_STATUS_OFFSET, display);
	boot->display = !(display & 1);
	if (boot->display)
		GU_VPR_READ(GU_DISPLAY_VPR_POLICY_OFFSET, boot->display_policy);
#undef GU_VPR_READ
	ret = gu_memory_decode_range(range, &memory);
	if (ret)
		return ret;
	start = (u64)(boot->low >> 4) << 12;
	end = ((u64)(boot->high >> 4) << 12 | 0xfffffull) + 1;
	saved_start = (u64)(boot->handoff & 0xfffff) << 20;
	saved_end = saved_start + ((u64)(size & 0xffff) << 20);
	tail = (u64)(sync & 0xffff) << 20;
	/* This is the devinit reservation, not a protected-content allocation.
	 * Match NVIDIA's BSI identity check and the native partial-scrub lock. */
	if ((boot->handoff & GU_VPR_HANDOFF_MASK) != GU_VPR_HANDOFF_DEVINIT ||
	    !start || start >= end || (start & 0xfffff) || saved_end < tail ||
	    start != saved_start || end != saved_end - tail || end > memory.physical_bytes ||
	    ((u64)(lock_low >> 4) << 12) != (256ull << 20) ||
	    ((u64)(lock_high >> 4) << 12) + 4096 != end ||
	    end != memory.physical_bytes -
		(memory.physical_bytes > (12ull << 30) ? 768ull << 20 : 256ull << 20))
		return -EPERM;
	boot->pending = true;
	return 0;
}

int gu_vpr_release(struct device *dev, void __iomem *bar, resource_size_t len,
		   const struct gu_vpr_boot *boot)
{
	struct gu_reg_write writes[5];
	struct gu_write_table table = { .entries = writes };
	u32 low, high, control, handoff, policy = 0;
	int ret;

	if (!boot->pending)
		return 0;
	ret = cold_boot(bar, len);
	if (ret)
		return ret;
#define GU_VPR_READ(reg, value) do { ret = vpr_read(bar, len, (reg), &(value)); \
	if (ret) return ret; } while (0)
	GU_VPR_READ(GU_VPR_ADDR_LO_OFFSET, low);
	GU_VPR_READ(GU_VPR_ADDR_HI_OFFSET, high);
	GU_VPR_READ(GU_VPR_CONTROL_OFFSET, control);
	GU_VPR_READ(GU_VPR_BOOT_HANDOFF_OFFSET, handoff);
	if (boot->display)
		GU_VPR_READ(GU_DISPLAY_VPR_POLICY_OFFSET, policy);
#undef GU_VPR_READ
	if (low != boot->low || high != boot->high || control != boot->control ||
	    handoff != boot->handoff || (boot->display && policy != boot->display_policy))
		return -EBUSY;
	writes[table.count++] = (struct gu_reg_write)
		GU_WRITE_MASK(GU_VPR_BOOT_HANDOFF_OFFSET, GU_VPR_HANDOFF_TAKING_OVER,
			      ~GU_VPR_HANDOFF_MASK, GU_VPR_HANDOFF_MASK);
	/* Keep the positive LO: HI=0 makes the range empty without relying on
	 * oversized sentinel writes being truncated by the address register. */
	writes[table.count++] = (struct gu_reg_write) GU_WRITE(GU_VPR_ADDR_HI_OFFSET, 0);
	writes[table.count++] = (struct gu_reg_write)
		GU_WRITE_MASK(GU_VPR_CONTROL_OFFSET, 0, ~GU_VPR_IN_USE, GU_VPR_IN_USE);
	if (boot->display)
		writes[table.count++] = (struct gu_reg_write)
			GU_WRITE_MASK(GU_DISPLAY_VPR_POLICY_OFFSET, 0x10, ~0x67u, 0x77);
	/* Publish completion last; preserve the saved capacity and versions. */
	writes[table.count++] = (struct gu_reg_write)
		GU_WRITE_MASK(GU_VPR_BOOT_HANDOFF_OFFSET, GU_VPR_HANDOFF_DONE,
			      ~GU_VPR_HANDOFF_MASK, GU_VPR_HANDOFF_MASK);
	ret = gu_host_writes(dev, bar, len, &table, "boot VPR release");
	if (!ret)
		dev_info(dev, "gpu_unlock: boot VPR released after ECC scrub\n");
	return ret;
}
