/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_ECC_H
#define GPU_UNLOCK_ECC_H

#include <linux/types.h>

struct device;
struct gu_ecc_recipe;

int gu_ecc_decode(const u8 *een, size_t size, bool *enabled);
int gu_ecc_read_mode(struct device *dev, void __iomem *bar, resource_size_t len,
		     const struct gu_ecc_recipe *recipe, bool *enabled);

/* Caller must quiesce firmware and retire its framebuffer state first.
 * Scrub the final memory range when ECC is enabled; never retry a trigger. */
int gu_ecc_scrub(struct device *dev, void __iomem *bar, resource_size_t len,
		 u64 usable_bytes);
void gu_ecc_log(struct device *dev, void __iomem *bar,
		resource_size_t len, const char *stage);

#endif
