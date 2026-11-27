// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe Gen2 speed-path programming for cards whose link is fused to Gen1.
 *
 * The register sequence is data (struct gu_pcie_gen2 in recipes.h) because it
 * is card-specific; this file only performs the read-modify-write ops, the
 * absolute writes and the PCIe link retrain. Sequence ported from
 * jdowning100/cmpunlocker rejoin16 (nv.c) with the retrain retry from
 * Wh1stle05/cmp90hx 0017.
 *
 * The pre-driver pass trains the link, but the stock driver's GSP-RM init
 * re-programs it back to Gen1, so the same pass is repeated by a delayed work
 * after the driver has probed the card (see gen2_delay_ms).
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "gen2.h"
#include "recipes.h"

static unsigned int gen2_delay_ms = 30000;
module_param(gen2_delay_ms, uint, 0444);
MODULE_PARM_DESC(gen2_delay_ms,
		 "delay before repeating the PCIe Gen2 pass after a card is unlocked (0 disables it)");

#define GEN2_RETRAIN_ROUNDS	5
#define GEN2_RETRAIN_POLLS	10
#define GEN2_RETRAIN_POLL_MS	100
#define GEN2_LATE_RETRIES	30
#define GEN2_LATE_RETRY_MS	2000

/* One work item per card, kept until module exit. The work never detaches
 * itself; gu_pcie_gen2_cancel() cancels it and drops the reference.
 */
struct gen2_late {
	struct list_head node;
	struct delayed_work work;
	struct pci_dev *pdev;
	const struct gu_pcie_gen2 *cfg;
	unsigned int tries;
};

static LIST_HEAD(gen2_late_list);
static DEFINE_MUTEX(gen2_late_lock);

/* Program the speed path and retrain. Returns 0 when the link is at 5.0 GT/s. */
int gu_pcie_gen2_config(struct pci_dev *pdev, const struct gu_pcie_gen2 *cfg)
{
	struct pci_dev *upstream;
	void __iomem *bar;
	u16 gpu_ctl2 = 0, up_ctl2 = 0, up_ctl = 0, link_status = 0;
	u32 val, ecc, xve;
	int ret, round, i;
	bool status_valid = false;

	if (!pdev || !cfg)
		return -EINVAL;
	upstream = pdev->bus ? pdev->bus->self : NULL;
	if (!upstream)
		return -ENODEV;

	ret = pci_enable_device_mem(pdev);
	if (ret)
		return ret;

	/* Refcounted: at boot this brings the card up, in the late pass it
	 * only bumps nvidia's count and the disable below restores it.
	 */

	bar = pci_iomap(pdev, 0, 0);
	if (!bar) {
		pci_disable_device(pdev);
		return -ENOMEM;
	}

	/* The speed path is gated by the privilege masks. The recipe opens both
	 * through the payload's privileged writes; without them, poking the
	 * LINK_CONFIG_0/PRIV_MISC_1 block can wedge the XVE logic.
	 */
	ecc = readl(bar + cfg->ecc_mask);
	xve = readl(bar + cfg->xve_mask);
	if (ecc != 0xffffffffu || xve != 0xffffffffu) {
		dev_warn(&pdev->dev,
			"gpu_unlock: Gen2 masks not open (ecc=%#x xve=%#x), skipping\n",
			ecc, xve);
		pci_iounmap(pdev, bar);
		pci_disable_device(pdev);
		return -EAGAIN;
	}

	val = readl(bar + cfg->priv_misc_1);
	writel((val | cfg->priv_misc_1_set) & ~cfg->priv_misc_1_clr,
	       bar + cfg->priv_misc_1);
	writel(readl(bar + cfg->cya_0) & ~cfg->cya_0_clr, bar + cfg->cya_0);
	for (i = 0; i < ARRAY_SIZE(cfg->xp3g); i++)
		if (cfg->xp3g[i].addr)
			writel(cfg->xp3g[i].value, bar + cfg->xp3g[i].addr);
	val = readl(bar + cfg->link_config_0);
	writel((val & ~cfg->link_config_0_mask) | cfg->link_config_0_value,
	       bar + cfg->link_config_0);
	writel(cfg->ltssm_value, bar + cfg->ltssm);
	/* Read back to flush the write before the retrain. */
	(void)readl(bar + cfg->ltssm);

	pci_iounmap(pdev, bar);
	pci_disable_device(pdev);

	/* Let LINK_CONFIG_0 settle before touching the capability. */
	msleep(50);

	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKCTL2, &gpu_ctl2);
	if (ret)
		goto capability_error;
	ret = pcie_capability_read_word(upstream, PCI_EXP_LNKCTL2, &up_ctl2);
	if (ret)
		goto capability_error;
	gpu_ctl2 = (gpu_ctl2 & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_5_0GT;
	up_ctl2 = (up_ctl2 & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_5_0GT;
	ret = pcie_capability_write_word(pdev, PCI_EXP_LNKCTL2, gpu_ctl2);
	if (ret)
		goto capability_error;
	ret = pcie_capability_write_word(upstream, PCI_EXP_LNKCTL2, up_ctl2);
	if (ret)
		goto capability_error;

	for (round = 0; round < GEN2_RETRAIN_ROUNDS; round++) {
		int poll;

		ret = pcie_capability_read_word(upstream, PCI_EXP_LNKCTL, &up_ctl);
		if (ret)
			goto capability_error;
		ret = pcie_capability_write_word(upstream, PCI_EXP_LNKCTL,
						 up_ctl | PCI_EXP_LNKCTL_RL);
		if (ret)
			goto capability_error;
		for (poll = 0; poll < GEN2_RETRAIN_POLLS; poll++) {
			msleep(GEN2_RETRAIN_POLL_MS);
			ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &link_status);
			if (ret)
				continue;
			status_valid = true;
			if ((link_status & PCI_EXP_LNKSTA_CLS) >=
			    PCI_EXP_LNKSTA_CLS_5_0GB) {
				dev_info(&pdev->dev,
					 "gpu_unlock: PCIe link trained to Gen%u\n",
					 link_status & PCI_EXP_LNKSTA_CLS);
				return 0;
			}
		}
		dev_dbg(&pdev->dev,
			"gpu_unlock: PCIe retrain round %d still Gen1 (status=%#x)\n",
			round, link_status);
	}

	if (status_valid)
		dev_warn(&pdev->dev,
			 "gpu_unlock: PCIe retrain finished without Gen2 (status=%#x)\n",
			 link_status);
	else
		dev_warn(&pdev->dev,
			 "gpu_unlock: PCIe retrain finished without a link status read\n");
	return status_valid ? -EIO : (ret ? ret : -EIO);

capability_error:
	dev_warn(&pdev->dev, "gpu_unlock: PCIe capability access failed (%d)\n", ret);
	return ret;
}

static void gen2_late_work(struct work_struct *work)
{
	struct gen2_late *late = container_of(to_delayed_work(work),
					      struct gen2_late, work);
	struct pci_dev *pdev = late->pdev;
	bool ready;
	int rc;

	pci_dev_lock(pdev);
	if (!pci_device_is_present(pdev)) {
		pci_dev_unlock(pdev);
		pci_warn(pdev, "gpu_unlock: late PCIe Gen2 skipped, device gone\n");
		return;
	}
	ready = pdev->current_state == PCI_D0 &&
		pdev->driver && !strcmp(pdev->driver->name, "nvidia");
	if (!ready) {
		pci_dev_unlock(pdev);
		if (late->tries++ < GEN2_LATE_RETRIES) {
			queue_delayed_work(system_freezable_wq, &late->work,
					   msecs_to_jiffies(GEN2_LATE_RETRY_MS));
			return;
		}
		pci_warn(pdev, "gpu_unlock: late PCIe Gen2 skipped, nvidia not ready\n");
		return;
	}
	rc = gu_pcie_gen2_config(pdev, late->cfg);
	pci_dev_unlock(pdev);
	if (rc)
		pci_err(pdev, "gpu_unlock: late PCIe Gen2 failed (%d)\n", rc);
	else
		pci_info(pdev, "gpu_unlock: late PCIe Gen2 applied\n");
}

/* Repeat the pass once the stock driver has finished its own link setup. */
void gu_pcie_gen2_schedule(struct pci_dev *pdev, const struct gu_pcie_gen2 *cfg)
{
	struct gen2_late *late;

	if (!pdev || !cfg || !gen2_delay_ms)
		return;
	late = kzalloc(sizeof(*late), GFP_KERNEL);
	if (!late)
		return;
	late->pdev = pci_dev_get(pdev);
	late->cfg = cfg;
	INIT_DELAYED_WORK(&late->work, gen2_late_work);
	mutex_lock(&gen2_late_lock);
	list_add_tail(&late->node, &gen2_late_list);
	mutex_unlock(&gen2_late_lock);
	queue_delayed_work(system_freezable_wq, &late->work,
			   msecs_to_jiffies(gen2_delay_ms));
}

/* Called from module exit only. Init cannot overlap exit, so no entry can be
 * added while the list is walked; cancel_delayed_work_sync() waits for a work
 * that is already running.
 */
void gu_pcie_gen2_cancel(void)
{
	struct gen2_late *late, *next;

	mutex_lock(&gen2_late_lock);
	list_for_each_entry_safe(late, next, &gen2_late_list, node) {
		cancel_delayed_work_sync(&late->work);
		pci_dev_put(late->pdev);
		list_del(&late->node);
		kfree(late);
	}
	mutex_unlock(&gen2_late_lock);
}
