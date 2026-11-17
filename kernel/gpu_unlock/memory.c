// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/timekeeping.h>

#include "host.h"
#include "memory.h"
#include "recipes.h"
#include "registers.h"

int gu_memory_decode_range(u32 range, struct gu_memory_info *info)
{
	u64 physical;

	/* Supported recipes use the lower range only. Reject other encodings
	 * rather than silently underestimating the scrub coverage. */
	if (!info || (range & ~0x400003ffu) || !((range >> 4) & 0x3f))
		return -ERANGE;
	physical = (u64)((range >> 4) & 0x3f) << ((range & 0xf) + 20);
	info->physical_bytes = physical;
	info->usable_bytes = range & (1u << 30) ? physical / 16 * 15 : physical;
	return 0;
}

static int memory_read(void __iomem *bar, resource_size_t len, u32 offset, u32 *value)
{
	if (!bar || (u64)offset + 4 > len)
		return -ERANGE;
	*value = readl(bar + offset);
	if (*value == ~0u || (*value & 0xffff0000u) == 0xbadf0000u)
		return -EIO;
	return 0;
}

/* CSTATUS reports usable MiB, including the inline-ECC reduction. Enumerate
 * active channels just as FWSEC does; a broadcast value times a channel count
 * would lose floorsweep and per-channel capacity information. */
static int gddr_capacity(void __iomem *bar, resource_size_t len, u64 *bytes)
{
	u32 fbps, channels, per_fbp, fbp_disabled, channel_disabled, value;
	u64 mib = 0;
	unsigned int fbp, j;
	int ret;

#define GU_MEMORY_READ(reg, dst) do { ret = memory_read(bar, len, (reg), &(dst)); \
	if (ret) return ret; } while (0)
	GU_MEMORY_READ(GU_MEMORY_FBP_COUNT_OFFSET, fbps);
	GU_MEMORY_READ(GU_MEMORY_CHANNEL_COUNT_OFFSET, channels);
	GU_MEMORY_READ(GU_MEMORY_CHANNELS_PER_FBP_OFFSET, per_fbp);
	GU_MEMORY_READ(GU_MEMORY_FBP_DISABLED_OFFSET, fbp_disabled);
	GU_MEMORY_READ(GU_MEMORY_CHANNEL_DISABLED_OFFSET, channel_disabled);
	fbps &= 0x1f;
	channels &= 0x1f;
	per_fbp &= 0x1f;
	if (!fbps || !channels || !per_fbp || fbps * per_fbp != channels)
		return -ERANGE;
	for (fbp = 0; fbp < fbps; fbp++) {
		if (fbp_disabled & (1u << fbp))
			continue;
		for (j = 0; j < per_fbp; j++) {
			unsigned int channel = fbp * per_fbp + j;

			if (channel_disabled & (1u << channel))
				continue;
			GU_MEMORY_READ(GU_MEMORY_CHANNEL_STATUS(channel), value);
			if (!(value & 0x1ffff))
				return -ERANGE;
			mib += value & 0x1ffff;
		}
	}
#undef GU_MEMORY_READ
	if (mib < 256)
		return -ERANGE;
	*bytes = mib << 20;
	return 0;
}

/* Move only reservations anchored to the old top. An unrelated lock extending
 * beyond the new usable range is unsafe, so fail before writing anything. */
static int gddr_reservations(u64 old_top, u64 new_top, u32 *vga, u32 *lo, u32 *hi)
{
	u64 delta = old_top - new_top;
	u64 base, low = (u64)(*lo >> 4) << 12;
	u64 high = (u64)(*hi >> 4) << 12;

	if (*vga & (1u << 3)) {
		base = (u64)(*vga >> 8) << 16;
		if (base >= old_top)
			return -ERANGE;
		if (old_top - base <= 256ull << 20) {
			if (base < delta)
				return -ERANGE;
			*vga = (u32)((base - delta) >> 16) << 8 | (*vga & 0xff);
		} else if (base >= new_top) {
			return -ERANGE;
		}
	}
	if (low <= high) {
		u64 top = (high + 0xffff) & ~0xffffull;
		u64 scrub_tail = old_top > (12ull << 30) ? 768ull << 20 : 256ull << 20;

		/* FWSEC's partial scrub protects [256 MiB, FB top - tail),
		 * with a 256 MiB tail through 12 GiB and 768 MiB above that.
		 * Firmware is now quiesced and a full NISO scrub is mandatory
		 * before handoff, so retire this exact boot-time guard. It is
		 * not a persistent end-of-FB reservation. Clearing HI while
		 * retaining the positive LO makes the range empty (LO > HI). */
		if (low == (256ull << 20) && high + 4096 == old_top - scrub_tail) {
			*hi &= 0xf;
		} else if (top == old_top) {
			if (low < delta)
				return -ERANGE;
			*lo = (u32)((low - delta) >> 8) | (*lo & 0xf);
			*hi = (u32)((high - delta) >> 8) | (*hi & 0xf);
		} else if (high >= new_top) {
			return -ERANGE;
		}
	}
	return 0;
}

static int gddr_finalize(struct device *dev, void __iomem *bar, resource_size_t len,
			 const struct gu_family *family, u32 range,
			 struct gu_memory_info *info)
{
	struct gu_reg_write writes[6];
	struct gu_write_table table = { .entries = writes };
	u32 before[ARRAY_SIZE(writes)];
	bool publish_size = family->memory == GU_MEMORY_GDDR;
	size_t count = publish_size ? ARRAY_SIZE(writes) : ARRAY_SIZE(writes) - 1;
	ktime_t deadline = ktime_add_us(ktime_get(), 100000);
	u64 bytes, expected = info->physical_bytes / 16 * 15;
	u32 readout, control, published, mirror, vga, lo, hi;
	unsigned int i;
	int ret;

	/* VGA and MMU reservation address fields cover at most one TiB. */
	if (info->physical_bytes > 1ull << 40)
		return -ERANGE;
#define GU_MEMORY_READ(reg, dst) do { ret = memory_read(bar, len, (reg), &(dst)); \
	if (ret) return ret; } while (0)
	GU_MEMORY_READ(family->feat->ecc_readout, readout);
	GU_MEMORY_READ(GU_FBPA_ECC_CONTROL_OFFSET, control);
	if ((readout & 0x18000) != 0x18000 || (control & 0x41) != 1) {
		dev_err(dev, "gpu_unlock: inline ECC not active readout=%#x control=%#x\n",
			readout, control);
		return -EIO;
	}
	/* Allow bounded propagation, but never manufacture capacity by blindly
	 * subtracting 1/16 from the old firmware scratch. */
	for (;;) {
		ret = gddr_capacity(bar, len, &bytes);
		if (ret)
			return ret;
		if (bytes == expected)
			break;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			dev_err(dev, "gpu_unlock: inline ECC capacity mismatch physical=%#llx expected=%#llx controllers=%#llx\n",
				(unsigned long long)info->physical_bytes,
				(unsigned long long)expected, (unsigned long long)bytes);
			return -EIO;
		}
		usleep_range(1000, 2000);
	}
	/* Turing publishes capacity through LOCAL_MEMORY_RANGE, without the
	 * Ampere GFW size scratch. Use its current range as the reservation top. */
	published = info->usable_bytes >> 20;
	if (publish_size)
		GU_MEMORY_READ(GU_GFW_FB_SIZE_OFFSET, published);
	GU_MEMORY_READ(GU_MEMORY_RANGE_MIRROR_OFFSET, mirror);
	GU_MEMORY_READ(GU_VGA_WORKSPACE_BASE_OFFSET, vga);
	GU_MEMORY_READ(GU_MMU_LOCK_ADDR_LO_OFFSET, lo);
	GU_MEMORY_READ(GU_MMU_LOCK_ADDR_HI_OFFSET, hi);
#undef GU_MEMORY_READ
	if (((u64)published << 20) != info->physical_bytes &&
	    ((u64)published << 20) != bytes)
		return -ERANGE;
	before[0] = range;
	before[1] = mirror;
	before[2] = hi;
	before[3] = lo;
	before[4] = vga;
	before[5] = published;
	ret = gddr_reservations((u64)published << 20, bytes, &vga, &lo, &hi);
	if (ret) {
		dev_err(dev, "gpu_unlock: unsafe inline ECC reservations old_mib=%#x new_mib=%#llx vga=%#x mmu=%#x:%#x\n",
			published, (unsigned long long)(bytes >> 20), vga, hi, lo);
		return ret;
	}
	range |= 1u << 30;
	writes[0] = (struct gu_reg_write)GU_WRITE(GU_LOCAL_MEMORY_RANGE_OFFSET, range);
	writes[1] = (struct gu_reg_write)GU_WRITE(GU_MEMORY_RANGE_MIRROR_OFFSET, range);
	writes[2] = (struct gu_reg_write)GU_WRITE(GU_MMU_LOCK_ADDR_HI_OFFSET, hi);
	writes[3] = (struct gu_reg_write)GU_WRITE(GU_MMU_LOCK_ADDR_LO_OFFSET, lo);
	writes[4] = (struct gu_reg_write)GU_WRITE(GU_VGA_WORKSPACE_BASE_OFFSET, vga);
	writes[5] = (struct gu_reg_write)GU_WRITE(GU_GFW_FB_SIZE_OFFSET, bytes >> 20);
	/* Leave unrelated or disabled reservations alone. An already-reconciled
	 * range needs verification only, including on a repeated invocation. */
	for (i = 0; i < count; i++)
		if (writes[i].value != before[i])
			writes[table.count++] = writes[i];
	ret = gu_host_writes(dev, bar, len, &table, "memory");
	if (!ret)
		info->usable_bytes = bytes;
	return ret;
}

int gu_memory_finalize(struct device *dev, void __iomem *bar, resource_size_t len,
		       const struct gu_family *family, bool ecc_enabled,
		       struct gu_memory_info *info)
{
	struct gu_memory_info result = { };
	u32 range, published;
	int ret;

	if (!family || !info)
		return -EINVAL;
	ret = memory_read(bar, len, GU_LOCAL_MEMORY_RANGE_OFFSET, &range);
	if (!ret)
		ret = gu_memory_decode_range(range, &result);
	if (ret)
		return ret;
	if (ecc_enabled && family->vram == GU_VRAM_GDDR) {
		ret = gddr_finalize(dev, bar, len, family, range, &result);
		if (ret)
			return ret;
	} else if (family->memory == GU_MEMORY_GDDR) {
		ret = memory_read(bar, len, GU_GFW_FB_SIZE_OFFSET, &published);
		if (ret)
			return ret;
		if (((u64)published << 20) != result.usable_bytes)
			return -ERANGE;
	}
	if (result.usable_bytes < 256ull << 20)
		return -ERANGE;
	*info = result;
	dev_info(dev, "gpu_unlock: stage=memory physical=%#llx usable=%#llx ecc=%u\n",
		 (unsigned long long)info->physical_bytes,
		 (unsigned long long)info->usable_bytes, ecc_enabled);
	return 0;
}
