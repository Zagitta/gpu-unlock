/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_PCIE_H
#define GPU_UNLOCK_PCIE_H

#include <linux/types.h>

struct gu_family;
struct pci_dev;

/* Cap on the advertised BAR1 size ladder, 2^N MB; negative (the default)
 * leaves the recipe's ladder alone. Set as gpu_unlock.bar1_max_exp. */
extern int bar1_max_exp;

/* Verify the P2P read override installed by the privileged recipe. Disable
 * the trap if its effective readback does not match. */
int gu_pcie_p2p_check(struct pci_dev *pdev, void __iomem *bar, resource_size_t len);

/* Apply the family's relink table, then set the target link speed of the card
 * and its upstream bridge to family->link_speed and retrain the link from the
 * bridge. bar maps len bytes of BAR0 from offset 0. The caller holds
 * pci_dev_lock. Returns 0 once the link runs at that generation or faster. */
int gu_pcie_relink(struct pci_dev *pdev, void __iomem *bar, resource_size_t len,
		   const struct gu_family *family);

/* Apply the family's bar1 table, then resize BAR1 to the largest size the
 * bridge window fits, as nvidia's nv_resize_pcie_bars does. On failure BAR1
 * goes back to its original size, so the GPU never decodes more than the
 * host assigned. Does nothing when firmware forbids moving BARs. The card
 * must have no driver bound. */
int gu_pcie_bar1_resize(struct pci_dev *pdev, void __iomem *bar, resource_size_t len,
			const struct gu_family *family);

#endif
