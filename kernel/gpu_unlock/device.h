/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_DEVICE_H
#define GPU_UNLOCK_DEVICE_H

struct pci_dev;
struct gu_card;

/* Resolved once during enumeration. The coordinator owns the PCI reference.
 * Firmware files and launch procedure come from recipe->family. Round-scoped
 * firmware assets are never stored here. */
struct gu_device {
	struct pci_dev *pdev;
	const struct gu_card *recipe;
};

#endif
