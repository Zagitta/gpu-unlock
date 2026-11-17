// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "host.h"
#include "recipes.h"

static bool pri_error(u32 value)
{
	return (value & 0xffff0000u) == 0xbadf0000u;
}

/* Returns 0, or the error of the first required entry that failed. */
static int apply(struct device *dev, void __iomem *bar, resource_size_t len,
		 const struct gu_write_table *table, const char *stage,
		 u32 *saved, size_t *written)
{
	size_t i;

	for (i = 0; i < table->count; i++) {
		const struct gu_reg_write *entry = &table->entries[i];
		u32 before, value, observed;

		if (!entry->check_mask)
			return -EINVAL;
		if ((u64)entry->addr + 4 > len) {
			dev_err(dev, "gpu_unlock: %s write %zu/%zu: %#x outside BAR0\n",
				stage, i + 1, table->count, entry->addr);
			return -ERANGE;
		}
		before = readl(bar + entry->addr);
		if (before == ~0u && !entry->optional) {
			dev_err(dev, "gpu_unlock: %s write %zu/%zu: %#x not responding\n",
				stage, i + 1, table->count, entry->addr);
			return -ENODEV;
		}
		value = (before & entry->keep) | entry->value;
		saved[i] = before;
		writel(value, bar + entry->addr);
		*written = i + 1;
		observed = readl(bar + entry->addr);
		if ((observed & entry->check_mask) == (value & entry->check_mask) &&
		    !pri_error(observed) && (observed != ~0u || entry->check_mask == ~0u)) {
			dev_dbg(dev, "%s write %zu/%zu: %#x %#x -> %#x\n",
				stage, i + 1, table->count, entry->addr, before, observed);
			continue;
		}
		if (entry->optional) {
			dev_info(dev, "gpu_unlock: optional %s write %zu/%zu target=%#x before=%#x wrote=%#x observed=%#x\n",
				 stage, i + 1, table->count, entry->addr, before, value, observed);
			continue;
		}
		dev_err(dev, "gpu_unlock: %s write %zu/%zu target=%#x before=%#x wrote=%#x expected=%#x observed=%#x\n",
			stage, i + 1, table->count, entry->addr, before, value, value, observed);
		return -EIO;
	}
	return 0;
}

int gu_host_writes(struct device *dev, void __iomem *bar, resource_size_t len,
		   const struct gu_write_table *table, const char *stage)
{
	size_t written = 0;
	u32 *saved;
	int ret;

	if (!table->count)
		return 0;
	saved = kcalloc(table->count, sizeof(*saved), GFP_KERNEL);
	if (!saved)
		return -ENOMEM;
	ret = apply(dev, bar, len, table, stage, saved, &written);
	/* Put back what the table already changed, newest first, so the card is
	 * left as it was rather than half configured. */
	if (ret && written) {
		dev_err(dev, "gpu_unlock: %s restoring %zu earlier writes\n", stage, written);
		while (written--)
			writel(saved[written], bar + table->entries[written].addr);
	}
	kfree(saved);
	return ret;
}
