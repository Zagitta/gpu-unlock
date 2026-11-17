/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_INFOROM_H
#define GPU_UNLOCK_INFOROM_H

#include <linux/types.h>

struct device;

/* Read-only JFFS access for single-block objects (up to 112 bytes, e.g. EEN).
 * -ENOENT means absent; damaged/unsupported data is a separate error. */
int gu_inforom_object(const u8 *image, size_t size, const char name[3],
		      u8 *object, size_t object_size);
int gu_inforom_read(struct device *dev, void __iomem *bar, resource_size_t len, u32 carveout_reg,
		    const char name[3], u8 *object, size_t object_size);
void gu_inforom_log_ga100(struct device *dev, void __iomem *bar,
			  resource_size_t len, const char *stage);

#endif
