/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_VPR_H
#define GPU_UNLOCK_VPR_H

#include <linux/types.h>

struct device;

struct gu_vpr_boot {
	u32 low, high, control, handoff, display_policy;
	bool pending, display;
};

/* GA102 devinit reservations, including after power-loss resume. Snapshot
 * ownership before memory finalization retires the boot MMU lock. Release
 * is allowed only after the caller's full ECC scrub. */
int gu_vpr_prepare(void __iomem *bar, resource_size_t len, struct gu_vpr_boot *boot);
int gu_vpr_release(struct device *dev, void __iomem *bar, resource_size_t len,
		   const struct gu_vpr_boot *boot);

#endif
