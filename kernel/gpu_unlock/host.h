/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_HOST_H
#define GPU_UNLOCK_HOST_H

#include <linux/types.h>

struct device;
struct gu_write_table;

/* Apply a table of host BAR0 writes in order and read each back. bar maps
 * len bytes of BAR0 from offset 0. A mismatch on an optional entry is logged;
 * on any other entry it stops the table and restores every register the
 * table already wrote. */
int gu_host_writes(struct device *dev, void __iomem *bar, resource_size_t len,
		   const struct gu_write_table *table, const char *stage);

#endif
