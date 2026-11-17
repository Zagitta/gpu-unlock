// SPDX-License-Identifier: GPL-2.0-only
/* GA100 HBM idle policy. No RM calls and no ownership of PMU counters. */
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/string.h>

#include "hbm.h"
#include "registers.h"

#define FBPA_BASE       0x00900000u
#define FBPA_STRIDE     0x00004000u
#define FBPA_CFG1       0x0204u
#define REFRESH_PLM     0x0168u
#define REFRESH         0x02a0u
#define REFRESH_MASK    0x00007ff8u
#define SOFTWARE_REF    0x0310u
#define PLL_PLM         0x3c7cu
#define PLL_CFG         0x3c90u
#define PLL_COEFF       0x3c98u
#define PLL_LOCK        0x20u
#define PLL_BYPASS      0x400u
#define NDIV_MASK       0xff00u
#define PLDIV_MASK      0x3f0000u
#define CLOCK_MASK      (NDIV_MASK | PLDIV_MASK)
#define PLL_CFG_MC2     0x0098bc90u
#define PLL_COEFF_MC2   0x0098bc98u
#define PRI_FENCE       0x001211fcu
#define FB_STOP_OUTPUT  0x0010a7c0u
#define FB_STOP_INPUT   0x0010a7c4u
#define FB_STOP_SET     0x0010a7e0u
#define FB_STOP_CLEAR   0x0010a7e4u
#define FB_STOP         (1u << 2)
#define COUNT_MASK      0x7fffffffu
/* Stay well below a 31-bit counter wrap even at the highest PMU clock. */
#define MAX_SAMPLE_NS   1000000000ull

static const u32 counter_addr[] = {
	0x10be40, 0x10be80, 0x10bec0, 0x10bfc0, 0x10be00,
};
/* GR, all nine copy engines, and framebuffer requests. */
static const u32 required[] = { 0x00380081, 0x00700000, 0x00000007 };

static bool readable(u32 value)
{
	return value != ~0u && (value & 0xfff00000u) != 0xbad00000u;
}

static int problem(struct gu_hbm *hbm, int error, const char *reason, u32 reg, u32 value)
{
	hbm->reason = reason;
	hbm->reg = reg;
	hbm->value = value;
	return error;
}

bool gu_hbm_booted(void __iomem *bar)
{
	u32 boot = readl(bar + GU_GFW_BOOT_OFFSET);
	u32 gsp = readl(bar + GU_GSP_RISCV_STATUS_OFFSET);

	return readable(boot) && readable(gsp) && (boot & 0xff) == 0xff &&
	       ((gsp & 7) == 3 || (gsp & 7) == 5);
}

void gu_hbm_init(struct gu_hbm *hbm)
{
	memset(hbm, 0, sizeof(*hbm));
	hbm->policy = (struct gu_hbm_policy) {
		.ndiv = GU_HBM_IDLE_NDIV, .pldiv = GU_HBM_IDLE_PLDIV,
		.refresh = GU_HBM_IDLE_REFRESH, .idle_ms = GU_HBM_IDLE_NS / 1000000,
		.mode = GU_HBM_AUTO,
	};
}

void gu_hbm_reset(struct gu_hbm *hbm)
{
	hbm->baseline = false;
	/* Keep the saved profile until idle settings and our stop are cleared. */
	if (!hbm->idle && !hbm->fb_stopped)
		hbm->profile = false;
}

static u32 fbpa(unsigned int index)
{
	return FBPA_BASE + index * FBPA_STRIDE;
}

static u32 idle_coeff(const struct gu_hbm *hbm)
{
	return (hbm->busy_coeff & ~CLOCK_MASK) |
		(hbm->policy.ndiv << 8) | (hbm->policy.pldiv << 16);
}

static int capture_profile(struct gu_hbm *hbm, void __iomem *bar)
{
	u32 coeff, cfg, mask = 0, first = 0;
	unsigned int i;

	for (i = 0; i < 24; i++) {
		u32 base = fbpa(i), geometry = readl(bar + base + FBPA_CFG1);
		u32 plm;

		if (!readable(geometry) || !geometry)
			continue;
		plm = readl(bar + base + REFRESH_PLM);
		if (!(plm & GU_PLM_HOST_WRITE) || (plm & 0xfff00000u) == 0xbad00000u)
			return problem(hbm, -EAGAIN, "refresh access closed", base + REFRESH_PLM, plm);
		hbm->busy_refresh[i] = readl(bar + base + REFRESH);
		if (!readable(hbm->busy_refresh[i]) || !(hbm->busy_refresh[i] & REFRESH_MASK))
			return problem(hbm, -EAGAIN, "invalid refresh", base + REFRESH, hbm->busy_refresh[i]);
		if (!mask)
			first = base;
		mask |= 1u << i;
	}
	if (!mask)
		return problem(hbm, -EAGAIN, "no accessible memory controllers", FBPA_BASE + FBPA_CFG1, 0);
	cfg = readl(bar + first + PLL_CFG);
	coeff = readl(bar + first + PLL_COEFF);
	i = readl(bar + first + PLL_PLM);
	if (!readable(cfg) || (cfg & (PLL_LOCK | 1)) != (PLL_LOCK | 1) ||
	    (cfg & ((1u << 7) | (1u << 10))))
		return problem(hbm, -EAGAIN, "HBM PLL not ready", first + PLL_CFG, cfg);
	if (!readable(coeff) || !(coeff & 0xff) || !(coeff & 0x3f0000))
		return problem(hbm, -EAGAIN, "invalid HBM PLL coefficient", first + PLL_COEFF, coeff);
	if (!(i & GU_PLM_HOST_WRITE) || (i & 0xfff00000u) == 0xbad00000u)
		return problem(hbm, -EAGAIN, "HBM PLL access closed", first + PLL_PLM, i);
	hbm->fbpas = mask;
	hbm->first_fbpa = first;
	hbm->busy_coeff = coeff;
	hbm->idle_coeff = idle_coeff(hbm);
	hbm->profile = true;
	return 0;
}

/* Shorten refresh intervals before increasing the clock, lengthen them after
 * decreasing it. Preserve every field outside the refresh interval. */
static int refresh(struct gu_hbm *hbm, void __iomem *bar, bool idle, bool before)
{
	unsigned int i;

	for (i = 0; i < 24; i++) {
		u32 addr, value, target, old;

		if (!(hbm->fbpas & (1u << i)))
			continue;
		addr = fbpa(i) + REFRESH;
		value = readl(bar + addr);
		if (!readable(value))
			return problem(hbm, -EIO, "refresh read failed", addr, value);
		old = value & REFRESH_MASK;
		target = idle ? hbm->policy.refresh << 3 : hbm->busy_refresh[i] & REFRESH_MASK;
		if (target == old || (target < old) != before)
			continue;
		writel((value & ~REFRESH_MASK) | target, bar + addr);
		value = readl(bar + addr);
		if (!readable(value) || (value & REFRESH_MASK) != target)
			return problem(hbm, -EIO, "refresh readback mismatch", addr, value);
	}
	return 0;
}

/* MC2 addresses the VBIOS-configured serial-priv master group. */
static void pll_write(void __iomem *bar, u32 reg, u32 value)
{
	writel(value, bar + reg);
	writel(0, bar + PRI_FENCE);
	/* NVIDIA's HBM sequence documents a serial-priv fence ordering hole. */
	ndelay(20);
}

static int pll_wait(struct gu_hbm *hbm, void __iomem *bar, u32 coeff,
		    bool bypass, const char *phase)
{
	unsigned int i, poll;
	u32 expected = PLL_LOCK | 1 | (bypass ? PLL_BYPASS : 0);

	for (i = 0; i < 24; i++) {
		u32 reg = fbpa(i) + PLL_CFG, value = 0;

		if (!(hbm->fbpas & (1u << i)))
			continue;
		for (poll = 0; poll < 200000; poll++) {
			reg = fbpa(i) + PLL_CFG;
			value = readl(bar + reg);
			if (!readable(value) ||
			    (value & (PLL_LOCK | 1 | PLL_BYPASS | (1u << 7))) != expected)
				continue;
			reg = fbpa(i) + PLL_COEFF;
			value = readl(bar + reg);
			if (value == coeff)
				break;
		}
		if (poll == 200000)
			return problem(hbm, -ETIMEDOUT, phase, reg, value);
	}
	return 0;
}

static int set_clock(struct gu_hbm *hbm, void __iomem *bar, bool idle)
{
	u32 old = readl(bar + hbm->first_fbpa + PLL_COEFF);
	u32 cfg = readl(bar + hbm->first_fbpa + PLL_CFG);
	u32 value = idle ? hbm->idle_coeff : hbm->busy_coeff;
	bool bypass = (old ^ value) & PLDIV_MASK;
	unsigned int i;
	int ret;

	if (old != hbm->busy_coeff && old != hbm->idle_coeff)
		return problem(hbm, -EIO, "HBM PLL configuration changed", hbm->first_fbpa + PLL_COEFF, old);
	if (!readable(cfg) || (cfg & (PLL_LOCK | 1 | PLL_BYPASS | (1u << 7))) != (PLL_LOCK | 1))
		return problem(hbm, -EIO, "HBM PLL not ready for transition", hbm->first_fbpa + PLL_CFG, cfg);
	if (value == old)
		return 0;
	if (bypass) {
		/* Isolate the post-divider from the active output. Keep the VCO
		 * enabled and retain the trained memory timings and delay lines. */
		pll_write(bar, PLL_CFG_MC2, cfg | PLL_BYPASS);
		ret = pll_wait(hbm, bar, old, true, "HBM bypass entry timeout");
		if (ret)
			goto rollback;
		udelay(2);
	}
	pll_write(bar, PLL_COEFF_MC2, value);
	for (i = 0; i < 500; i++)
		readl(bar + hbm->first_fbpa + PLL_CFG);
	ret = pll_wait(hbm, bar, value, bypass, "HBM PLL lock/coefficient timeout");
	if (ret)
		goto rollback;
	if (bypass) {
		pll_write(bar, PLL_CFG_MC2, cfg);
		udelay(2);
		ret = pll_wait(hbm, bar, value, false, "HBM bypass exit timeout");
		if (ret)
			goto rollback;
	}
	return 0;
rollback:
	/* Restore both coefficient fields and clock selection on either leg. */
	if (bypass) {
		pll_write(bar, PLL_CFG_MC2, cfg | PLL_BYPASS);
		udelay(2);
	}
	pll_write(bar, PLL_COEFF_MC2, old);
	for (i = 0; i < 500; i++)
		readl(bar + hbm->first_fbpa + PLL_CFG);
	if (bypass) {
		pll_write(bar, PLL_CFG_MC2, cfg);
		udelay(2);
	}
	return ret;
}

static int fb_wait(struct gu_hbm *hbm, void __iomem *bar, bool stopped)
{
	u32 value = 0;
	unsigned int i;

	for (i = 0; i < 1000; i++) {
		value = readl(bar + FB_STOP_INPUT);
		if (!readable(value))
			return problem(hbm, -EIO, "HBM FB stop ACK unreadable", FB_STOP_INPUT, value);
		if (!!(value & FB_STOP) == stopped)
			return 0;
		udelay(1);
	}
	return problem(hbm, -ETIMEDOUT, stopped ? "HBM FB stop ACK timeout" :
		       "HBM FB resume ACK timeout", FB_STOP_INPUT, value);
}

static int fb_stop(struct gu_hbm *hbm, void __iomem *bar)
{
	u32 request = readl(bar + FB_STOP_OUTPUT), ack = readl(bar + FB_STOP_INPUT);

	if (!readable(request) || !readable(ack))
		return problem(hbm, -EIO, "HBM FB stop state unreadable",
			       !readable(request) ? FB_STOP_OUTPUT : FB_STOP_INPUT,
			       !readable(request) ? request : ack);
	/* Reuse our own held request after a failed transition. Never clear
	 * a stop asserted by firmware or another owner. */
	if (request & FB_STOP) {
		if (hbm->fb_stopped)
			return fb_wait(hbm, bar, true);
		return problem(hbm, -EBUSY, "HBM FB stop already owned", FB_STOP_OUTPUT, request);
	}
	/* Our previous release may have cleared the request without receiving
	 * the ACK falling edge. Reassert the stop so rollback can still run. */
	if ((ack & FB_STOP) && !hbm->fb_stopped)
		return problem(hbm, -EBUSY, "HBM FB stop ACK still set", FB_STOP_INPUT, ack);
	hbm->fb_stopped = true;
	writel(FB_STOP, bar + FB_STOP_SET);
	request = readl(bar + FB_STOP_OUTPUT);
	if (!readable(request) || !(request & FB_STOP))
		return problem(hbm, -EIO, "HBM FB stop request did not latch", FB_STOP_OUTPUT, request);
	return fb_wait(hbm, bar, true);
}

static int fb_start(struct gu_hbm *hbm, void __iomem *bar)
{
	u32 request;
	unsigned int i;
	int ret;

	/* NVIDIA's FB-start sequence refreshes first to keep memory clocks
	 * running across release of the stop request. */
	for (i = 0; i < 24; i++)
		if (hbm->fbpas & (1u << i))
			writel(1, bar + fbpa(i) + SOFTWARE_REF);
	writel(0, bar + PRI_FENCE);
	ndelay(20);
	writel(FB_STOP, bar + FB_STOP_CLEAR);
	request = readl(bar + FB_STOP_OUTPUT);
	if (!readable(request) || (request & FB_STOP))
		return problem(hbm, -EIO, "HBM FB resume request did not clear", FB_STOP_OUTPUT, request);
	ret = fb_wait(hbm, bar, false);
	if (!ret)
		hbm->fb_stopped = false;
	return ret;
}

static int transition(struct gu_hbm *hbm, void __iomem *bar, bool idle)
{
	int ret = fb_stop(hbm, bar), resumed;

	if (!ret)
		ret = refresh(hbm, bar, idle, true);
	if (!ret)
		ret = set_clock(hbm, bar, idle);
	if (!ret)
		ret = refresh(hbm, bar, idle, false);
	if (!hbm->fb_stopped)
		return ret;
	if (ret) {
		u32 coeff = readl(bar + hbm->first_fbpa + PLL_COEFF);
		int ready;

		/* Keep traffic stopped if rollback could not restore a known,
		 * locked profile. A later restore can reuse our held request. */
		if (coeff != hbm->busy_coeff && coeff != hbm->idle_coeff)
			return problem(hbm, -EIO, "HBM FB resume blocked by unknown PLL", hbm->first_fbpa + PLL_COEFF, coeff);
		ready = pll_wait(hbm, bar, coeff, false, "HBM FB resume blocked by PLL timeout");
		if (ready)
			return ready;
	}
	resumed = fb_start(hbm, bar);
	return resumed ? resumed : ret;
}

int gu_hbm_restore(struct gu_hbm *hbm, void __iomem *bar)
{
	int ret;

	if (!hbm->idle && !hbm->fb_stopped)
		return 0;
	if (!gu_hbm_booted(bar))
		return -EAGAIN;
	ret = transition(hbm, bar, false);
	if (!ret)
		hbm->idle = false;
	return ret;
}

static int enter_idle(struct gu_hbm *hbm, void __iomem *bar)
{
	int ret;

	/* MDIV is retained, so compare N/P without rounding the MHz value. */
	if (((hbm->busy_coeff & NDIV_MASK) >> 8) * hbm->policy.pldiv <
	    hbm->policy.ndiv * ((hbm->busy_coeff & PLDIV_MASK) >> 16))
		return problem(hbm, -EINVAL, "HBM idle target exceeds busy clock", 0, 0);
	hbm->idle_coeff = idle_coeff(hbm);
	hbm->idle = true;
	ret = transition(hbm, bar, true);
	if (ret) {
		gu_hbm_restore(hbm, bar);
		hbm->baseline = false;
	}
	return ret;
}

static bool read_counter(struct gu_hbm *hbm, void __iomem *bar,
			 unsigned int slot, struct gu_hbm_counter *c)
{
	u32 before[5], after[5];
	unsigned int i;

	for (i = 0; i < 5; i++)
		before[i] = readl(bar + counter_addr[i] + slot * 4);
	c->count = readl(bar + 0x10bf80 + slot * 4);
	c->delta = 0;
	for (i = 0; i < 5; i++)
		after[i] = readl(bar + counter_addr[i] + slot * 4);
	if (memcmp(before, after, sizeof(before)) || after[3] != 2 ||
	    (after[4] & ~COUNT_MASK) || (c->count & ~COUNT_MASK)) {
		problem(hbm, -EAGAIN, "activity counter unavailable", counter_addr[3] + slot * 4, after[3]);
		return false;
	}
	memcpy(c->mask, after, sizeof(c->mask));
	c->control = after[3];
	c->threshold = after[4];
	return true;
}

static bool discover_counters(struct gu_hbm *hbm, void __iomem *bar)
{
	u32 *covered = hbm->coverage;
	unsigned int i, bank;

	hbm->counters = 0;
	memset(covered, 0, sizeof(hbm->coverage));
	for (i = 0; i < 16; i++) {
		struct gu_hbm_counter c;
		bool useful = false;

		if (!read_counter(hbm, bar, i, &c))
			continue;
		for (bank = 0; bank < 3; bank++)
			if (c.mask[bank] & required[bank] & ~covered[bank])
				useful = true;
		if (!useful)
			continue;
		hbm->counter[i] = c;
		hbm->counters |= 1u << i;
		for (bank = 0; bank < 3; bank++)
			covered[bank] |= c.mask[bank];
	}
	for (bank = 0; bank < 3; bank++)
		if ((covered[bank] & required[bank]) != required[bank]) {
			problem(hbm, -EAGAIN, "activity counter coverage missing", counter_addr[bank], covered[bank]);
			return false;
		}
	hbm->reason = NULL;
	return true;
}

int gu_hbm_poll(struct gu_hbm *hbm, void __iomem *bar, u64 now)
{
	bool busy = false;
	unsigned int i;
	int ret;

	hbm->reason = NULL;
	hbm->active_counters = 0;
	if (!gu_hbm_booted(bar)) {
		u32 boot = readl(bar + GU_GFW_BOOT_OFFSET);

		gu_hbm_reset(hbm);
		if (!readable(boot) || (boot & 0xff) != 0xff)
			return problem(hbm, -EAGAIN, "waiting for GFW boot", GU_GFW_BOOT_OFFSET, boot);
		return problem(hbm, -EAGAIN, "waiting for GFW/GSP boot", GU_GSP_RISCV_STATUS_OFFSET,
			       readl(bar + GU_GSP_RISCV_STATUS_OFFSET));
	}
	if (!hbm->profile) {
		ret = capture_profile(hbm, bar);
		if (ret)
			return ret;
	}
	if (hbm->policy.mode == GU_HBM_BUSY)
		return gu_hbm_restore(hbm, bar);
	if (hbm->policy.mode == GU_HBM_IDLE) {
		if (!hbm->baseline) {
			ret = gu_hbm_restore(hbm, bar);
			if (ret)
				return ret;
		}
		ret = hbm->idle ? 0 : enter_idle(hbm, bar);
		hbm->baseline = !ret;
		return ret;
	}
	if (!hbm->baseline) {
		ret = gu_hbm_restore(hbm, bar);
		if (ret)
			return ret;
		if (!discover_counters(hbm, bar))
			return -EAGAIN;
		hbm->baseline = true;
		hbm->sampled = hbm->quiet_since = now;
		return 0;
	}
	if (now < hbm->sampled || now - hbm->sampled > MAX_SAMPLE_NS) {
		problem(hbm, -EAGAIN, "activity sample gap", 0, 0);
		goto unknown;
	}
	for (i = 0; i < 16; i++) {
		struct gu_hbm_counter c, *previous = &hbm->counter[i];

		if (!(hbm->counters & (1u << i)))
			continue;
		if (!read_counter(hbm, bar, i, &c))
			goto unknown;
		if (memcmp(c.mask, previous->mask, sizeof(c.mask)) ||
		    c.control != previous->control || c.threshold != previous->threshold) {
			problem(hbm, -EAGAIN, "activity counter configuration changed",
				counter_addr[0] + i * 4, c.mask[0]);
			goto unknown;
		}
		c.delta = (c.count - previous->count) & COUNT_MASK;
		if (c.delta)
			hbm->active_counters |= 1u << i;
		busy |= c.delta != 0;
		*previous = c;
	}
	hbm->sampled = now;
	if (busy) {
		hbm->quiet_since = now;
		return gu_hbm_restore(hbm, bar);
	}
	if (!hbm->idle && now - hbm->quiet_since >= (u64)hbm->policy.idle_ms * 1000000)
		return enter_idle(hbm, bar);
	return 0;
unknown:
	hbm->baseline = false;
	ret = gu_hbm_restore(hbm, bar);
	return ret ? ret : -EAGAIN;
}
