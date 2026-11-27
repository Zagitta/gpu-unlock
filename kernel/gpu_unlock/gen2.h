/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_GEN2_H
#define GPU_UNLOCK_GEN2_H

struct pci_dev;
struct gu_pcie_gen2;

/* Program the PCIe Gen2 speed path described by cfg and retrain the link.
 * Returns 0 when the link reached 5.0 GT/s, negative otherwise.
 */
int gu_pcie_gen2_config(struct pci_dev *pdev, const struct gu_pcie_gen2 *cfg);

/* Repeat the pass gen2_delay_ms later, after the stock driver has finished
 * its own GSP-RM link setup (which reverts the pre-driver retrain).
 */
void gu_pcie_gen2_schedule(struct pci_dev *pdev, const struct gu_pcie_gen2 *cfg);

/* Cancel the pending repeats (module exit). */
void gu_pcie_gen2_cancel(void);

#endif
