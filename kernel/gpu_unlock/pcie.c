// SPDX-License-Identifier: GPL-2.0-only
/*
 * Retrain from the upstream port after applying the card's link policy.
 * Retry a completed Gen1 attempt promptly, with a slower final fallback for
 * cold-boot behavior. The PCI core's retrain helper is not exported to modules.
 * BAR1 resize after the BAR1 unlock, following nvidia's
 * nv_resize_pcie_bars.
 */
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/timekeeping.h>

#include "host.h"
#include "pcie.h"
#include "recipes.h"
#include "registers.h"

#define GU_RETRAIN_PASSES       3
#define GU_RETRAIN_SETTLE_US    50000
#define GU_RETRAIN_TIMEOUT_US   2000000
#define GU_RETRAIN_FALLBACK_MS  500
#define GU_RELINK_SETTLE_MS  200  /* the strap shadow latches after the writes */

int gu_pcie_p2p_check(struct pci_dev *pdev, void __iomem *bar, resource_size_t len)
{
	if (len < GU_P2P_SECURE_OFFSET + 4)
		return -ERANGE;
	if (readl(bar + GU_P2P_SECURE_OFFSET) != 0) {
		writel(0, bar + GU_P2P_TRAP_ACTION_OFFSET);
		pci_err(pdev, "gpu_unlock: P2P secure-option read override failed\n");
		return -EIO;
	}
	pci_info(pdev, "gpu_unlock: PCIe P2P mailbox permissions and read override enabled\n");
	return 0;
}

static int link_status(struct pci_dev *pdev, struct pci_dev *bridge,
		       u16 *ep, u16 *up)
{
	int ret;

	*ep = *up = 0xffff;
	ret = pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, up);
	if (ret) {
		*up = 0xffff;
		return pcibios_err_to_errno(ret);
	}
	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, ep);
	if (ret)
		*ep = 0xffff;
	return ret ? pcibios_err_to_errno(ret) : 0;
}

static bool link_idle(u16 ep, u16 up, bool active_reporting)
{
	return ep != 0xffff && up != 0xffff &&
	       (ep & PCI_EXP_LNKSTA_CLS) && (up & PCI_EXP_LNKSTA_CLS) &&
	       !(up & PCI_EXP_LNKSTA_LT) &&
	       (!active_reporting || (up & PCI_EXP_LNKSTA_DLLLA));
}

/* gen == 0 waits for existing training before issuing a new request. After
 * a request, allow a short stable idle interval for LT assertion/status lag
 * before deciding that training finished at the wrong speed. The last pass
 * keeps the full observation budget for cold-boot transitions. */
static int wait_link(struct pci_dev *pdev, struct pci_dev *bridge,
		     unsigned int gen, bool active_reporting, bool quick,
		     u16 *ep, u16 *up)
{
	ktime_t start = ktime_get(), stable = start;
	u16 previous_ep = 0xffff, previous_up = 0xffff;
	bool settling = false;
	int ret;

	for (;;) {
		ret = link_status(pdev, bridge, ep, up);
		if (ret) {
			/* Endpoint config may disappear briefly during a retrain.
			 * Only tolerate that while the port confirms a transition. */
			if (ret != -ENODEV || *up == 0xffff)
				return ret;
			if (!(*up & PCI_EXP_LNKSTA_LT) &&
			    (!active_reporting || (*up & PCI_EXP_LNKSTA_DLLLA)))
				return ret;
		}
		if (gen && (*ep != previous_ep || *up != previous_up))
			dev_dbg(&pdev->dev, "gpu_unlock: link status target=Gen%u elapsed_us=%lld endpoint=%#x upstream=%#x\n",
				gen, (long long)ktime_us_delta(ktime_get(), start), *ep, *up);
		if (link_idle(*ep, *up, active_reporting)) {
			if (!gen || ((*ep & PCI_EXP_LNKSTA_CLS) >= gen &&
				     (*up & PCI_EXP_LNKSTA_CLS) >= gen))
				return 0;
			if (!settling || *ep != previous_ep || *up != previous_up) {
				stable = ktime_get();
				settling = true;
			}
			if (quick && ktime_us_delta(ktime_get(), stable) >= GU_RETRAIN_SETTLE_US)
				return -EAGAIN;
		} else {
			settling = false;
		}
		previous_ep = *ep;
		previous_up = *up;
		if (ktime_us_delta(ktime_get(), start) >= GU_RETRAIN_TIMEOUT_US)
			return -ETIMEDOUT;
		usleep_range(1000, 2000);
	}
}

static void log_link(struct pci_dev *pdev, struct pci_dev *bridge,
		     const char *stage, unsigned int pass, ktime_t start,
		     u16 ep, u16 up, int result)
{
	u16 ep_target = 0xffff, up_target = 0xffff;
	u16 ep_control = 0xffff, up_control = 0xffff;

	pcie_capability_read_word(pdev, PCI_EXP_LNKCTL2, &ep_target);
	pcie_capability_read_word(bridge, PCI_EXP_LNKCTL2, &up_target);
	pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &ep_control);
	pcie_capability_read_word(bridge, PCI_EXP_LNKCTL, &up_control);
	pci_info(pdev, "gpu_unlock: link %s pass=%u elapsed_us=%lld result=%d endpoint_status=%#x upstream_status=%#x endpoint_target=%#x upstream_target=%#x endpoint_control=%#x upstream_control=%#x\n",
		 stage, pass, (long long)ktime_us_delta(ktime_get(), start), result,
		 ep, up, ep_target, up_target, ep_control, up_control);
}

static void log_policy(struct pci_dev *pdev, void __iomem *bar,
		       resource_size_t len, unsigned int pass)
{
	if (len >= GU_XP_PL_LINK_RATE_OFFSET + 4)
		pci_info(pdev, "gpu_unlock: link policy pass=%u cap2=%#x xp_config=%#x xp_rates=%#x priv_misc1=%#x\n",
			 pass, readl(bar + GU_XVE_LINK_CAP_2_OFFSET),
			 readl(bar + GU_XP_LINK_CONFIG_0_OFFSET),
			 readl(bar + GU_XP_PL_LINK_RATE_OFFSET), readl(bar + GU_XVE_PRIV_MISC_1_OFFSET));
}

/* The endpoint advertises the speed it will accept; a Gen1-capable endpoint
 * keeps its Target Link Speed at Gen1, so the readback says whether the link
 * policy landed before any retrain is worth attempting. */
static int retrain(struct pci_dev *pdev, unsigned int gen,
		   void __iomem *bar, resource_size_t len)
{
	struct pci_dev *bridge = pci_upstream_bridge(pdev);
	ktime_t start = ktime_get(), attempt;
	u32 cap;
	u16 ep, up, ep_target, up_target;
	unsigned int pass;
	bool active_reporting;
	int ret;

	if (!bridge || !pci_is_pcie(pdev) || !pci_is_pcie(bridge))
		return -ENODEV;
	ret = pcie_capability_read_dword(bridge, PCI_EXP_LNKCAP, &cap);
	if (ret)
		return pcibios_err_to_errno(ret);
	active_reporting = cap & PCI_EXP_LNKCAP_DLLLARC;
	ret = wait_link(pdev, bridge, 0, active_reporting, false, &ep, &up);
	if (ret) {
		log_link(pdev, bridge, "initial-wait", 0, start, ep, up, ret);
		return ret;
	}
	if ((ep & PCI_EXP_LNKSTA_CLS) >= gen && (up & PCI_EXP_LNKSTA_CLS) >= gen)
		return 0;

	for (pass = 1; pass <= GU_RETRAIN_PASSES; pass++) {
		if (pass == GU_RETRAIN_PASSES) {
			msleep(GU_RETRAIN_FALLBACK_MS);
			ret = wait_link(pdev, bridge, 0, active_reporting, false, &ep, &up);
			log_link(pdev, bridge, "fallback-wait", pass, start, ep, up, ret);
			if (ret)
				return ret;
			if ((ep & PCI_EXP_LNKSTA_CLS) >= gen && (up & PCI_EXP_LNKSTA_CLS) >= gen)
				return 0;
		}
		/* Target Link Speed uses the same encoding as the generation. */
		ret = pcie_capability_clear_and_set_word(pdev, PCI_EXP_LNKCTL2,
							 PCI_EXP_LNKCTL2_TLS, gen);
		if (!ret)
			ret = pcie_capability_clear_and_set_word(bridge, PCI_EXP_LNKCTL2,
								 PCI_EXP_LNKCTL2_TLS, gen);
		if (ret)
			return pcibios_err_to_errno(ret);
		ret = pcie_capability_read_word(pdev, PCI_EXP_LNKCTL2, &ep_target);
		if (!ret)
			ret = pcie_capability_read_word(bridge, PCI_EXP_LNKCTL2, &up_target);
		if (ret)
			return pcibios_err_to_errno(ret);
		if ((ep_target & PCI_EXP_LNKCTL2_TLS) != gen ||
		    (up_target & PCI_EXP_LNKCTL2_TLS) != gen) {
			pci_warn(pdev, "gpu_unlock: target Gen%u did not stick (endpoint Gen%u, upstream Gen%u)\n",
				 gen, ep_target & PCI_EXP_LNKCTL2_TLS, up_target & PCI_EXP_LNKCTL2_TLS);
			return -EOPNOTSUPP;
		}
		/* LNKCTL2 writes may race with automatic training. Wait for that
		 * to finish before requesting another retrain, as PCI core does.
		 * Retrain Link is reserved on endpoints; only the port is triggered. */
		ret = wait_link(pdev, bridge, 0, active_reporting, false, &ep, &up);
		log_link(pdev, bridge, "request", pass, start, ep, up, ret);
		if (ret)
			return ret;
		if ((ep & PCI_EXP_LNKSTA_CLS) >= gen && (up & PCI_EXP_LNKSTA_CLS) >= gen)
			return 0;
		attempt = ktime_get();
		ret = pcie_capability_set_word(bridge, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_RL);
		if (ret)
			return pcibios_err_to_errno(ret);
		ret = wait_link(pdev, bridge, gen, active_reporting,
				pass < GU_RETRAIN_PASSES, &ep, &up);
		log_link(pdev, bridge, "complete", pass, attempt, ep, up, ret);
		log_policy(pdev, bar, len, pass);
		if (!ret) {
			pci_info(pdev, "gpu_unlock: link Gen%u on pass %u total_us=%lld\n",
				 gen, pass, (long long)ktime_us_delta(ktime_get(), start));
			return 0;
		}
		if (ret != -EAGAIN)
			break;
	}
	pci_warn(pdev, "gpu_unlock: link retrain failed result=%d endpoint_status=%#x upstream_status=%#x total_us=%lld\n",
		 ret, ep, up, (long long)ktime_us_delta(ktime_get(), start));
	return ret;
}

int gu_pcie_relink(struct pci_dev *pdev, void __iomem *bar, resource_size_t len,
		   const struct gu_family *family)
{
	u32 cap2;
	int ret;

	if (!family->link_speed)
		return 0;
	if ((u64)GU_XVE_LINK_CTRL_2_OFFSET + 4 > len)
		return -ERANGE;
	ret = gu_host_writes(&pdev->dev, bar, len, &family->relink, "link");
	if (ret)
		return ret;
	/* The strap shadow latches after the write burst. */
	msleep(GU_RELINK_SETTLE_MS);
	cap2 = readl(bar + GU_XVE_LINK_CAP_2_OFFSET);
	log_policy(pdev, bar, len, 0);
	/* LnkCap2 bit n advertises Gen n. */
	if (!(cap2 & (1u << family->link_speed))) {
		pci_warn(pdev, "gpu_unlock: endpoint LnkCap2 %#x does not advertise Gen%u\n",
			 cap2, family->link_speed);
		return -EOPNOTSUPP;
	}
	return retrain(pdev, family->link_speed, bar, len);
}

/* BAR3 shares BAR1's prefetchable bridge window, so it moves with it. */
#define GU_BAR1 1
#define GU_BAR3 3

static DEFINE_MUTEX(bar1_lock);
static const unsigned int resize_bars[] = { GU_BAR1, GU_BAR3 };

static int resize_bar1(struct pci_dev *pdev, int size)
{
#ifdef GU_RESIZE_EXCLUDE_BARS
	/* BAR0 is mapped by the unlock transaction and must not move. */
	return pci_resize_resource(pdev, GU_BAR1, size, BIT(0));
#else
	return pci_resize_resource(pdev, GU_BAR1, size);
#endif
}

static bool bars_assigned(struct pci_dev *pdev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(resize_bars); i++) {
		unsigned int bar = resize_bars[i];
		struct resource *res = &pdev->resource[bar];

		if (!(res->flags & IORESOURCE_MEM) ||
		    (res->flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED)) ||
		    !res->parent || !res->start || res->end < res->start ||
		    !pci_resource_len(pdev, bar))
			return false;
	}
	return true;
}

static int bar1_control(struct pci_dev *pdev)
{
	int pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_REBAR);
	unsigned int i, count;
	u32 ctrl;
	int ret;

	if (!pos)
		return -ENODEV;
	ret = pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	if (ret)
		return pcibios_err_to_errno(ret);
	count = (ctrl & PCI_REBAR_CTRL_NBAR_MASK) >> PCI_REBAR_CTRL_NBAR_SHIFT;
	for (i = 0; i < count; i++, pos += 8) {
		ret = pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
		if (ret)
			return pcibios_err_to_errno(ret);
		if ((ctrl & PCI_REBAR_CTRL_BAR_IDX) == GU_BAR1)
			return pos + PCI_REBAR_CTRL;
	}
	return -ENOENT;
}

/* The unlock changes the decoder before Linux can allocate its new window.
 * Restore the old size so failed PCI resize attempts roll back to that size. */
static int set_bar1_size(struct pci_dev *pdev, int control, int size)
{
	u32 ctrl, observed;
	int ret;

	ret = pci_read_config_dword(pdev, control, &ctrl);
	if (ret)
		return pcibios_err_to_errno(ret);
	ctrl = (ctrl & ~PCI_REBAR_CTRL_BAR_SIZE) | (size << PCI_REBAR_CTRL_BAR_SHIFT);
	ret = pci_write_config_dword(pdev, control, ctrl);
	if (!ret)
		ret = pci_read_config_dword(pdev, control, &observed);
	if (ret)
		return pcibios_err_to_errno(ret);
	return (observed & PCI_REBAR_CTRL_BAR_SIZE) ==
	       (ctrl & PCI_REBAR_CTRL_BAR_SIZE) ? 0 : -EIO;
}

static int restore_bars(struct pci_dev *pdev, int control, int size,
			const struct resource *saved)
{
	unsigned int i;
	int ret;

	ret = set_bar1_size(pdev, control, size);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(resize_bars); i++) {
		pci_release_resource(pdev, resize_bars[i]);
		if (pdev->resource[resize_bars[i]].parent)
			return -EBUSY;
	}
	/* Allocation failure may clear flags as well as the address and size.
	 * Restore only the geometry, never stale resource-tree links. */
	for (i = 0; i < ARRAY_SIZE(resize_bars); i++) {
		struct resource *res = &pdev->resource[resize_bars[i]];

		res->start = saved[i].start;
		res->end = saved[i].end;
		res->flags = saved[i].flags;
	}
	for (i = 0; i < ARRAY_SIZE(resize_bars); i++) {
		unsigned int bar = resize_bars[i], j;
		struct resource *res = &pdev->resource[bar];
		u64 address;

		ret = pci_claim_resource(pdev, bar);
		if (ret)
			return ret;
		/* pci_update_resource() is not exported to modules. */
		address = pci_bus_address(pdev, bar) | (res->flags & ~PCI_BASE_ADDRESS_MEM_MASK);
		for (j = 0; j < (res->flags & IORESOURCE_MEM_64 ? 2 : 1); j++) {
			int reg = PCI_BASE_ADDRESS_0 + (bar + j) * 4;
			u32 value = address >> (j * 32), observed;

			ret = pci_write_config_dword(pdev, reg, value);
			if (!ret)
				ret = pci_read_config_dword(pdev, reg, &observed);
			if (ret)
				return pcibios_err_to_errno(ret);
			if (observed != value)
				return -EIO;
			pdev->saved_config_space[reg / 4] = value;
		}
	}
	return bars_assigned(pdev) ? 0 : -ENODEV;
}

int gu_pcie_bar1_resize(struct pci_dev *pdev, void __iomem *bar, resource_size_t len,
			const struct gu_family *family)
{
	struct resource saved[ARRAY_SIZE(resize_bars)];
	int old, control, size, ret = 0, error;
	unsigned int i;
	u64 sizes;
	u16 cmd;

	if (!family->bar1.count)
		return 0;
	/* Different cards can resize the same upstream bridge windows. */
	mutex_lock(&bar1_lock);
	if (pci_find_host_bridge(pdev->bus)->preserve_config ||
	    !pci_rebar_get_possible_sizes(pdev, GU_BAR1)) {
		pci_info(pdev, "gpu_unlock: BAR1 cannot be resized here, left locked\n");
		goto out;
	}
	if (!bars_assigned(pdev)) {
		ret = -ENODEV;
		goto out;
	}
	old = pci_rebar_bytes_to_size(pci_resource_len(pdev, GU_BAR1));
	control = bar1_control(pdev);
	if (control < 0) {
		ret = control;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(resize_bars); i++)
		saved[i] = pdev->resource[resize_bars[i]];
	ret = pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	if (ret) {
		ret = pcibios_err_to_errno(ret);
		goto out;
	}
	ret = gu_host_writes(&pdev->dev, bar, len, &family->bar1, "bar1");
	if (ret)
		goto out;
	sizes = pci_rebar_get_possible_sizes(pdev, GU_BAR1);

	/* The kernel requires memory decoding off while BARs move. */
	ret = pci_write_config_word(pdev, PCI_COMMAND, cmd & ~PCI_COMMAND_MEMORY);
	if (ret) {
		ret = pcibios_err_to_errno(ret);
		goto out;
	}
	ret = restore_bars(pdev, control, old, saved);
	if (ret)
		goto out;
	for (size = fls64(sizes) - 1; size > old; size--) {
		if (!(sizes & BIT_ULL(size)))
			continue;
		/* Older kernels require both resources released by the caller. */
		for (i = 0; i < ARRAY_SIZE(resize_bars); i++)
			pci_release_resource(pdev, resize_bars[i]);
		ret = resize_bar1(pdev, size);
		if (!ret) {
			pci_assign_unassigned_bus_resources(pdev->bus);
			if (bars_assigned(pdev))
				break;
			ret = -ENOSPC;
		}
		/* Reclaim the working windows before another attempt. If none of
		 * the larger sizes fit, these are also the successful fallback. */
		error = restore_bars(pdev, control, old, saved);
		if (error) {
			ret = error;
			goto out;
		}
		if (ret != -ENOSPC)
			break;
		ret = 0;
	}
	error = pci_write_config_word(pdev, PCI_COMMAND, cmd);
	if (!ret && error)
		ret = pcibios_err_to_errno(error);
	if (!ret)
		pci_info(pdev, "gpu_unlock: BAR1 resized to %llu MiB\n",
			 (unsigned long long)(pci_resource_len(pdev, GU_BAR1) >> 20));
out:
	if (ret)
		pci_err(pdev, "gpu_unlock: BAR1 resize failed: %d\n", ret);
	mutex_unlock(&bar1_lock);
	return ret;
}
