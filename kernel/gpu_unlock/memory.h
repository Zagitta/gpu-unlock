/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_MEMORY_H
#define GPU_UNLOCK_MEMORY_H

#include <linux/types.h>

struct device;
struct gu_family;

enum gu_memory_kind {
	GU_MEMORY_RANGE, /* usable size decoded from the local range */
	GU_MEMORY_GDDR,  /* also verify/update GFW's published size */
};

struct gu_memory_info {
	u64 physical_bytes;
	u64 usable_bytes;
};

int gu_memory_decode_range(u32 range, struct gu_memory_info *info);

/* Firmware must be quiesced. Does not alter the temporary Booter layout.
 * Verify final capacity and reconcile firmware bookkeeping before PLM locks. */
int gu_memory_finalize(struct device *dev, void __iomem *bar, resource_size_t len,
		       const struct gu_family *family, bool ecc_enabled,
		       struct gu_memory_info *info);

#endif
