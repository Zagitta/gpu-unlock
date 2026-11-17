/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_PM_H
#define GPU_UNLOCK_PM_H

struct device;
struct pci_dev;
struct gu_runtime_pm;

/* A child PM device runs after its parent's PCI noirq restoration, before
 * device IRQs, ordinary driver resume and userspace restart. Registration and
 * removal must exclude system sleep. The caller owns data until removal. */
struct device *gu_pm_add(struct device *parent, int (*resume_noirq)(void *), void *data);
void gu_pm_remove(struct device *dev);

/* Install before a PCI driver binds, with system sleep excluded. The callback
 * runs with PCI access restored, before NVIDIA's runtime resume. Runtime PM
 * serializes it; it must not acquire the PCI device lock, which its caller
 * may already hold. Removal drains callbacks before releasing data. */
struct gu_runtime_pm *gu_runtime_pm_add(struct pci_dev *pdev,
				       int (*resume)(void *), void *data);
void gu_runtime_pm_remove(struct gu_runtime_pm *pm);

#endif
