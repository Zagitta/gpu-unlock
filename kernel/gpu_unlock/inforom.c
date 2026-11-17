// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "inforom.h"
#include "registers.h"

#define IFR_BLOCK_SIZE 128u
#define HEADER_SIZE 16u
#define PROM_SIZE 0x100000u
#define PROM_PAGE_SIZE 4096u

/* GA102 and later expose a second, paged aperture for larger EEPROMs. It
 * addresses physical EEPROM bytes, independently of ROM_WINDOW_XVE. */
static int read_prom(void __iomem *bar, resource_size_t len, u32 offset,
		     u32 size, u32 window, u8 *image)
{
	bool paged = offset >= PROM_SIZE || size > PROM_SIZE - offset;
	u32 saved = 0, word, window_offset = (window & 0xfff) << 8;
	size_t i;
	int ret = 0;

	if (paged) {
		u32 chip = (readl(bar + GU_PMC_BOOT_0_OFFSET) >> 20) & 0x1ff;

		if (chip < 0x172 || chip > 0x197 || offset + size > 0x1000000u ||
		    len < GU_PROM_2_DATA_OFFSET + PROM_PAGE_SIZE || len < GU_ROM_ADDR_OFFSET_2 + 4)
			return -ERANGE;
		saved = readl(bar + GU_ROM_ADDR_OFFSET_2);
		if (saved == ~0u || (saved & 0xffff0000u) == 0xbadf0000u)
			return -ENODEV;
	} else {
		if (offset < window_offset)
			return -ERANGE;
		offset -= window_offset;
		if ((u64)GU_PROM_DATA_OFFSET + offset + size > len)
			return -ERANGE;
	}
	for (i = 0; i < size; i += 4) {
		u32 address = GU_PROM_DATA_OFFSET + offset + i;
		unsigned int byte;

		if (paged) {
			if (!(i % PROM_PAGE_SIZE)) {
				u32 page = (offset + i) | 1u;

				writel(page, bar + GU_ROM_ADDR_OFFSET_2);
				if (readl(bar + GU_ROM_ADDR_OFFSET_2) != page) {
					ret = -EIO;
					break;
				}
			}
			address = GU_PROM_2_DATA_OFFSET + i % PROM_PAGE_SIZE;
		}
		word = readl(bar + address);
		for (byte = 0; byte < 4; byte++)
			image[i + byte] = word >> (byte * 8);
	}
	if (paged) {
		writel(saved, bar + GU_ROM_ADDR_OFFSET_2);
		if (readl(bar + GU_ROM_ADDR_OFFSET_2) != saved)
			return -EIO;
	}
	return ret;
}

static u32 le(const u8 *p, unsigned int bytes)
{
	u32 value = 0;
	unsigned int i;

	for (i = 0; i < bytes; i++)
		value |= (u32)p[i] << (8 * i);
	return value;
}

int gu_inforom_object(const u8 *image, size_t size, const char name[3],
		      u8 *object, size_t object_size)
{
	const u8 *best = NULL;
	bool damaged = false;
	size_t offset, i;

	if (!image || !object || !name || object_size < 8 ||
	    object_size > IFR_BLOCK_SIZE - HEADER_SIZE)
		return -EINVAL;
	if (size < 4096 || size % 4096 || memcmp(image, "JFFS", 4) ||
	    (le(image + 4, 4) != 1 && le(image + 4, 4) != 2) ||
	    le(image + 8, 4) != size)
		return -EBADMSG;
	for (offset = IFR_BLOCK_SIZE; offset < size; offset += IFR_BLOCK_SIZE) {
		const u8 *block = image + offset;
		u8 sum = 0;
		bool valid = true;

		/* Erased and obsolete nodes are not live objects. EEN may have
		 * several current journal nodes: take the newest valid version,
		 * as NVIDIA's filesystem does, rather than the last physical one. */
		if ((block[0] & 1) || !(block[0] & 2) || memcmp(block + 1, name, 3))
			continue;
		if ((block[0] & ~0x0f) || le(block + 4, 2) || block[6] != object_size ||
		    (!(block[0] & 4) && ((block[0] & 8) || block[7])))
			return -EBADMSG;
		for (i = 9; i < HEADER_SIZE; i++)
			if (block[i])
				return -EBADMSG;
		if (!(block[0] & 8)) {
			for (i = 0; i < IFR_BLOCK_SIZE; i++)
				sum += block[i];
			valid = !sum;
		}
		if (!valid) {
			damaged = true;
			continue;
		}
		if (best && ((best[0] ^ block[0]) & 4))
			return -EBADMSG;
		if (best && best[7] == block[7] &&
		    memcmp(best + HEADER_SIZE, block + HEADER_SIZE, object_size))
			return -EBADMSG;
		if (!best || block[7] > best[7])
			best = block;
	}
	if (!best)
		return damaged ? -EBADMSG : -ENOENT;
	if (memcmp(best + HEADER_SIZE, name, 3) ||
	    le(best + HEADER_SIZE + 5, 2) != object_size)
		return -EBADMSG;
	memcpy(object, best + HEADER_SIZE, object_size);
	return 0;
}

int gu_inforom_read(struct device *dev, void __iomem *bar, resource_size_t len, u32 carveout_reg,
		    const char name[3], u8 *object, size_t object_size)
{
	u32 bounds, window, offset, size;
	u8 *image;
	int ret;

	if (!bar || (u64)carveout_reg + 4 > len || len < GU_ROM_WINDOW_OFFSET + 4)
		return -ERANGE;
	bounds = readl(bar + carveout_reg);
	offset = (bounds >> 8) & 0xfff;
	size = (bounds >> 20) & 0xfff;
	if (bounds == ~0u || (bounds & 0xffff0000u) == 0xbadf0000u ||
	    !offset || offset == 0xfff || !size || size == 0xfff)
		return -ENODEV;
	offset <<= 12;
	size <<= 12;
	/* Scratch reports a physical EEPROM address. The reader compensates
	 * for ROM_WINDOW_XVE only when using the legacy aperture. */
	window = readl(bar + GU_ROM_WINDOW_OFFSET);
	if (window == ~0u || (window & 0xffff0000u) == 0xbadf0000u)
		return -ENODEV;
	image = kvmalloc(size, GFP_KERNEL);
	if (!image)
		return -ENOMEM;
	/* Both apertures support 32-bit reads without SPI transactions. Preserve
	 * the page selector when accessing InfoROM beyond the first MiB. */
	ret = read_prom(bar, len, offset, size, window, image);
	if (ret) {
		kvfree(image);
		return ret;
	}
	ret = bounds == readl(bar + carveout_reg) &&
	      window == readl(bar + GU_ROM_WINDOW_OFFSET) ?
		gu_inforom_object(image, size, name, object, object_size) : -EAGAIN;
	dev_dbg(dev, "gpu_unlock: inforom bounds=%#x window=%#x prom_offset=%#x size=%#x header=%#x:%#x:%#x result=%d\n",
		bounds, window, offset, size, le(image, 4), le(image + 4, 4), le(image + 8, 4), ret);
	if (ret && ret != -ENOENT)
		dev_err(dev, "gpu_unlock: inforom read failed: error=%d bounds=%#x window=%#x offset=%#x magic=%#x version=%#x size=%#x\n",
			ret, bounds, window, offset, le(image, 4), le(image + 4, 4), le(image + 8, 4));
	kvfree(image);
	return ret;
}

void gu_inforom_log_ga100(struct device *dev, void __iomem *bar,
			  resource_size_t len, const char *stage)
{
	u32 boot0, gfw, carveout, offset, size;
	bool valid;

	if (!bar || len < GU_INFOROM_CARVEOUT_OFFSET + 4) {
		dev_warn(dev, "gpu_unlock: inforom stage=%s BAR0 too small\n", stage);
		return;
	}
	boot0 = readl(bar + GU_PMC_BOOT_0_OFFSET);
	if (((boot0 >> 20) & 0x1ff) != 0x170) {
		dev_warn(dev, "gpu_unlock: inforom stage=%s GA100 not accessible boot0=%#x\n",
			 stage, boot0);
		return;
	}
	gfw = readl(bar + GU_GFW_BOOT_OFFSET);
	carveout = readl(bar + GU_INFOROM_CARVEOUT_OFFSET);
	offset = (carveout >> 8) & 0xfff;
	size = (carveout >> 20) & 0xfff;
	valid = (carveout & 0xffff0000u) != 0xbadf0000u &&
		offset && offset != 0xfff && size && size != 0xfff;
	dev_info(dev,
		 "gpu_unlock: inforom stage=%s boot0=%#x gfw_boot=%#x raw=%#x valid=%u offset=%#x size=%#x\n",
		 stage, boot0, gfw, carveout, valid,
		 valid ? offset << 12 : 0, valid ? size << 12 : 0);
}
