// SPDX-License-Identifier: GPL-2.0-only
/*
 * Retrain from the upstream port after applying the card's link policy.
 * Retry a completed Gen1 attempt promptly, with a slower final fallback for
 * cold-boot behavior. The PCI core's retrain helper is not exported to modules.
 * BAR1 resize after the BAR1 unlock, following nvidia's
 * nv_resize_pcie_bars.
 */
#include <linux/delay.h>
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

static int resize_bar1(struct pci_dev *pdev, int size)
{
#ifdef GU_RESIZE_EXCLUDE_BARS
	return pci_resize_resource(pdev, GU_BAR1, size, 0);
#else
	return pci_resize_resource(pdev, GU_BAR1, size);
#endif
}

static bool bars_assigned(struct pci_dev *pdev)
{
	return !(pci_resource_flags(pdev, GU_BAR1) & IORESOURCE_UNSET) &&
	       !(pci_resource_flags(pdev, GU_BAR3) & IORESOURCE_UNSET);
}

int gu_pcie_bar1_resize(struct pci_dev *pdev, void __iomem *bar, resource_size_t len,
			const struct gu_family *family)
{
	int old = pci_rebar_bytes_to_size(pci_resource_len(pdev, GU_BAR1));
	int size, ret;
	u64 sizes;
	u16 cmd;

	if (!family->bar1.count)
		return 0;
	if (pci_find_host_bridge(pdev->bus)->preserve_config ||
	    !pci_rebar_get_possible_sizes(pdev, GU_BAR1)) {
		pci_info(pdev, "gpu_unlock: BAR1 cannot be resized here, left locked\n");
		return 0;
	}
	ret = gu_host_writes(&pdev->dev, bar, len, &family->bar1, "bar1");
	if (ret)
		return ret;
	sizes = pci_rebar_get_possible_sizes(pdev, GU_BAR1);

	/* The kernel requires memory decoding off while BARs move. */
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	pci_write_config_word(pdev, PCI_COMMAND, cmd & ~PCI_COMMAND_MEMORY);
	pci_release_resource(pdev, GU_BAR1);
	pci_release_resource(pdev, GU_BAR3);
	for (size = fls64(sizes) - 1; size > old; size--)
		if ((sizes & BIT_ULL(size)) && !resize_bar1(pdev, size))
			break;
	/* Nothing larger fits: the decoder must still return to the old size. */
	ret = size > old ? 0 : resize_bar1(pdev, old);
	pci_assign_unassigned_bus_resources(pdev->bus);
	if (!bars_assigned(pdev) && size > old) {
		ret = resize_bar1(pdev, old);
		pci_assign_unassigned_bus_resources(pdev->bus);
	}
	pci_write_config_word(pdev, PCI_COMMAND, cmd);
	if (!ret && !bars_assigned(pdev))
		ret = -ENODEV;
	if (ret) {
		pci_err(pdev, "gpu_unlock: BAR1 resize failed: %d\n", ret);
		return ret;
	}
	pci_info(pdev, "gpu_unlock: BAR1 resized to %llu MiB\n",
		 (unsigned long long)(pci_resource_len(pdev, GU_BAR1) >> 20));
	return 0;
}
