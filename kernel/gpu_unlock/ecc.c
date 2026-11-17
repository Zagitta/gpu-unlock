// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/timekeeping.h>
#include <linux/string.h>

#include "diagnostic.h"
#include "ecc.h"
#include "inforom.h"
#include "recipes.h"
#include "registers.h"

int gu_ecc_decode(const u8 *een, size_t size, bool *enabled)
{
	size_t i;

	if (!een || !enabled || size != 112 || memcmp(een, "EEN\x01\x00\x70\x00", 7))
		return -EBADMSG;
	*enabled = false;
	/* NVIDIA uses the parity of the first nonzero dword. A completely
	 * consumed (all-zero) buffer also decodes to disabled. */
	for (i = 8; i < size; i += 4) {
		u32 word = (u32)een[i] | (u32)een[i + 1] << 8 |
			   (u32)een[i + 2] << 16 | (u32)een[i + 3] << 24;

		if (!word)
			continue;
		while (word) {
			*enabled = !*enabled;
			word &= word - 1;
		}
		break;
	}
	return 0;
}

int gu_ecc_read_mode(struct device *dev, void __iomem *bar, resource_size_t len,
		     const struct gu_ecc_recipe *recipe, bool *enabled)
{
	u8 een[112];
	u32 subrevision;
	int ret;

	*enabled = false;
	ret = gu_inforom_read(dev, bar, len, recipe->carveout_reg, "EEN", een, sizeof(een));
	if (!ret)
		ret = gu_ecc_decode(een, sizeof(een), enabled);
	if (ret && ret != -ENOENT) {
		dev_err(dev, "gpu_unlock: cannot read ECC request from InfoROM: %d\n", ret);
		return ret;
	}
	if (*enabled && recipe->min_subrevision) {
		if (len < GU_OPT_SUBREVISION_OFFSET + 4)
			return -ERANGE;
		subrevision = readl(bar + GU_OPT_SUBREVISION_OFFSET);
		if (subrevision == ~0u || (subrevision & 0xffff0000u) == 0xbadf0000u)
			return -EIO;
		if ((subrevision & 0xf) < recipe->min_subrevision) {
			dev_err(dev, "gpu_unlock: ECC recipe requires silicon subrevision >= %u; found %u\n",
				recipe->min_subrevision, subrevision & 0xf);
			return -EOPNOTSUPP;
		}
	}
	dev_info(dev, "gpu_unlock: ECC requested=%s source=%s\n",
		 *enabled ? "enabled" : "disabled", ret == -ENOENT ? "EEN absent" : "EEN");
	return 0;
}

static bool inaccessible(u32 value)
{
	return value == ~0u || (value & 0xffff0000u) == 0xbadf0000u;
}

static int scrub_read(struct device *dev, void __iomem *bar, u32 offset, u32 *value)
{
	*value = readl(bar + offset);
	if (!inaccessible(*value))
		return 0;
	dev_err(dev, "gpu_unlock: scrub register inaccessible offset=%#x value=%#x\n",
		offset, *value);
	return GU_DIAG(-EIO);
}

/* The scrubber is independent of the halted Falcons and needs no sysmem DMA.
 * Bound both the initial idle wait and the scrub itself; never replay a trigger
 * after a timeout or accept the reset DONE bit as proof of range coverage. */
static int wait_fb_scrub(struct device *dev, void __iomem *bar, const u32 *end)
{
	ktime_t deadline = ktime_add_us(ktime_get(), 5000000);
	u32 status, last = 0;
	unsigned int progress, next_progress = 0;
	int ret;

	for (;;) {
		ret = scrub_read(dev, bar, GU_NISO_SCRUB_STATUS_OFFSET, &status);
		if (ret)
			return ret;
		if (end) {
			ret = scrub_read(dev, bar, GU_NISO_SCRUB_LAST_OFFSET, &last);
			if (ret)
				return ret;
			/* LAST is the current page; reserve 100% for completion. */
			progress = min_t(u64, (u64)last * 100 / ((u64)*end + 1), 99);
			if ((status & 1) && last == *end)
				progress = 100;
			if (progress >= next_progress) {
				dev_info(dev,
					 "gpu_unlock: scrub progress=%u%% last=%#x end=%#x\n",
					 progress, last, *end);
				next_progress = (progress / 10 + 1) * 10;
			}
		}
		if ((status & 1) && (!end || last == *end))
			return 0;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			dev_err(dev, "gpu_unlock: scrub %s timeout status=%#x last=%#x end=%#x\n",
				end ? "completion" : "idle", status, last, end ? *end : 0);
			return GU_DIAG(-ETIMEDOUT);
		}
		usleep_range(1000, 2000);
	}
}

int gu_ecc_scrub(struct device *dev, void __iomem *bar, resource_size_t len,
		 u64 bytes)
{
	u32 end, first, last, value;
	const u32 offsets[] = { GU_NISO_SCRUB_START_OFFSET,
		GU_NISO_SCRUB_END_OFFSET, GU_NISO_SCRUB_PATTERN_OFFSET };
	u32 values[] = { 0, 0, 0 };
	unsigned int i;
	int ret;

	if (!bar || len < GU_NISO_SCRUB_FIRST_OFFSET + 4)
		return GU_DIAG(-ERANGE);
	if (!bytes || (bytes & 0xfff) || (bytes >> 12) > (u64)~0u)
		return GU_DIAG(-ERANGE);
	end = (bytes >> 12) - 1;
	values[1] = end;
	dev_info(dev, "gpu_unlock: scrubbing %#llx bytes of framebuffer\n",
		 (unsigned long long)bytes);
	ret = wait_fb_scrub(dev, bar, NULL);
	if (ret)
		return ret;
	/* Read back the setup before issuing the non-repeatable trigger. */
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		writel(values[i], bar + offsets[i]);
		ret = scrub_read(dev, bar, offsets[i], &value);
		if (ret)
			return ret;
		if (value != values[i]) {
			dev_err(dev, "gpu_unlock: scrub setup mismatch offset=%#x wrote=%#x observed=%#x\n",
				offsets[i], values[i], value);
			return GU_DIAG(-EIO);
		}
	}
	writel(1, bar + GU_NISO_SCRUB_TRIGGER_OFFSET); /* low to high */
	ret = wait_fb_scrub(dev, bar, &end);
	if (ret)
		return ret;
	ret = scrub_read(dev, bar, GU_NISO_SCRUB_FIRST_OFFSET, &first);
	if (!ret)
		ret = scrub_read(dev, bar, GU_NISO_SCRUB_LAST_OFFSET, &last);
	if (ret)
		return ret;
	if (first || last != end) {
		dev_err(dev, "gpu_unlock: scrub coverage mismatch first=%#x last=%#x end=%#x\n",
			first, last, end);
		return GU_DIAG(-EIO);
	}
	dev_info(dev,
		 "gpu_unlock: stage=scrub bytes=%#llx first=%#x last=%#x\n",
		 (unsigned long long)bytes, first, last);
	return 0;
}

void gu_ecc_log(struct device *dev, void __iomem *bar,
		resource_size_t len, const char *stage)
{
	u32 boot0, chip, readout, readout_reg, control, hbm;
	bool valid;

	if (!bar || len < GU_FBPA_HBM_CFG0_OFFSET + 4) {
		dev_warn(dev, "gpu_unlock: ecc stage=%s BAR0 too small\n", stage);
		return;
	}
	boot0 = readl(bar + GU_PMC_BOOT_0_OFFSET);
	chip = (boot0 >> 20) & 0x1ff;
	switch (chip) {
	case 0x162: case 0x164: case 0x166:
		readout_reg = GU_TU102_FEAT_ECC_READOUT_OFFSET;
		break;
	case 0x170: case 0x172: case 0x173: case 0x174: case 0x176:
	case 0x192: case 0x193: case 0x194: case 0x196: case 0x197:
		readout_reg = GU_FEAT_ECC_READOUT_OFFSET;
		break;
	default:
		dev_warn(dev, "gpu_unlock: ecc stage=%s chip not accessible boot0=%#x\n",
			 stage, boot0);
		return;
	}
	readout = readl(bar + readout_reg);
	control = readl(bar + GU_FBPA_ECC_CONTROL_OFFSET);
	if (chip != 0x170) {
		u32 status = readl(bar + GU_MEMORY_CHANNEL_STATUS(0));

		valid = !inaccessible(readout) && !inaccessible(control) &&
			!inaccessible(status);
		if (chip == 0x172) {
			u32 poison = readl(bar + GU_OPT_GLOBAL_POISON_EN_OFFSET);

			dev_info(dev,
				 "gpu_unlock: ecc stage=%s valid=%u readout=%#x control=%#x poison=%#x channel0=%#x\n",
				 stage, valid && !inaccessible(poison), readout, control, poison, status);
			return;
		}
		dev_info(dev,
			 "gpu_unlock: ecc stage=%s valid=%u readout=%#x control=%#x channel0=%#x ltc=%u dram=%u\n",
			 stage, valid, readout, control, status,
			 valid && !!(readout & (1u << 15)), valid && !!(readout & (1u << 16)));
		return;
	}
	hbm = readl(bar + GU_FBPA_HBM_CFG0_OFFSET);
	valid = !inaccessible(readout) && !inaccessible(control) && !inaccessible(hbm);
	/* Register observations do not establish that correction works. */
	dev_info(dev,
		 "gpu_unlock: ecc stage=%s valid=%u readout=%#x control=%#x hbm=%#x ltc=%u dram=%u sideband=%u hbm_ecc=%u\n",
		 stage, valid, readout, control, hbm,
		 valid && !!(readout & (1u << 15)), valid && !!(readout & (1u << 16)),
		 valid && !!(control & (1u << 6)), valid && !!(hbm & (1u << 9)));
}
