// SPDX-License-Identifier: GPL-2.0-only
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "pm.h"

struct gu_pm_device {
	struct device dev;
	struct completion released;
	int (*resume_noirq)(void *);
	void *data;
};

static int gu_pm_resume_noirq(struct device *dev)
{
	struct gu_pm_device *pm = container_of(dev, struct gu_pm_device, dev);
	unsigned int flags;
	int ret;

	/* Device IRQs are disabled, not CPU timer interrupts. Firmware uses
	 * polling; allocations must not reclaim through suspended storage. */
	flags = memalloc_noio_save();
	ret = pm->resume_noirq(pm->data);
	memalloc_noio_restore(flags);
	return ret;
}

static const struct dev_pm_ops gu_pm_ops = {
	.resume_noirq = gu_pm_resume_noirq,
};

static void gu_pm_release(struct device *dev)
{
	complete(&container_of(dev, struct gu_pm_device, dev)->released);
}

static const struct device_type gu_pm_type = {
	.name = "gpu-unlock",
	.pm = &gu_pm_ops,
	.release = gu_pm_release,
};

struct device *gu_pm_add(struct device *parent, int (*resume_noirq)(void *), void *data)
{
	struct gu_pm_device *pm;
	int ret;

	pm = kzalloc(sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return ERR_PTR(-ENOMEM);
	pm->resume_noirq = resume_noirq;
	pm->data = data;
	init_completion(&pm->released);
	device_initialize(&pm->dev);
	pm->dev.parent = parent;
	pm->dev.type = &gu_pm_type;
	/* The PM core waits for the parent and for every noirq callback before
	 * enabling device IRQs. Different cards can initialize concurrently. */
	device_enable_async_suspend(&pm->dev);
	ret = dev_set_name(&pm->dev, "gpu-unlock");
	if (!ret)
		ret = device_add(&pm->dev);
	if (ret) {
		put_device(&pm->dev);
		wait_for_completion(&pm->released);
		kfree(pm);
		return ERR_PTR(ret);
	}
	return &pm->dev;
}

void gu_pm_remove(struct device *dev)
{
	struct gu_pm_device *pm;

	if (!dev)
		return;
	pm = container_of(dev, struct gu_pm_device, dev);
	device_unregister(dev);
	/* The device type and release callback belong to this module. Drain
	 * outstanding device references before its code or callback data go away. */
	wait_for_completion(&pm->released);
	kfree(pm);
}

struct gu_runtime_pm {
	struct dev_pm_domain domain;
	struct pci_dev *pdev;
	int (*pci_resume)(struct device *dev);
	int (*resume)(void *data);
	void *data;
};

static void runtime_irq(unsigned int irq, bool enable)
{
	if (!irq)
		return;
	if (enable)
		enable_irq(irq);
	else
		disable_irq(irq);
}

/* NVIDIA retains its IRQ allocations across runtime suspend. Keep its handlers
 * out of the temporary firmware, just as system resume's noirq phase does. */
static void runtime_irqs(struct pci_dev *pdev, bool enable)
{
#ifdef CONFIG_PCI_MSI
	unsigned int i;

#ifdef GU_MSI_GET_VIRQ
	if (pdev->msi_enabled || pdev->msix_enabled) {
		/* Configuration space may still be inaccessible in D3cold. Look
		 * up allocated vectors in software, including sparse MSI-X tables. */
		unsigned int count = pdev->msix_enabled ? PCI_MSIX_FLAGS_QSIZE + 1 : 32;

		for (i = 0; i < count; i++)
			runtime_irq(msi_get_virq(&pdev->dev, i), enable);
		return;
	}
#else
	if (pdev->msi_enabled || pdev->msix_enabled) {
		struct msi_desc *desc;

		for_each_pci_msi_entry(desc, pdev) {
			if (desc->irq)
				for (i = 0; i < desc->nvec_used; i++)
					runtime_irq(desc->irq + i, enable);
		}
		return;
	}
#endif
#endif
	runtime_irq(pdev->irq, enable);
}

static int gu_runtime_resume(struct device *dev)
{
	struct gu_runtime_pm *pm = container_of(dev->pm_domain, struct gu_runtime_pm, domain);
	struct pci_dev *pdev = pm->pdev;
	unsigned int flags;
	bool saved;
	int ret;

	/* The hook can outlive a driver unbind. Other drivers use ordinary PCI PM. */
	if (!dev->driver || strcmp(dev->driver->name, "nvidia"))
		return pm->pci_resume(dev);
	if (dev->power.irq_safe)
		return -EOPNOTSUPP;

	flags = memalloc_noio_save();
	ret = pci_set_power_state(pdev, PCI_D0);
	if (!ret) {
		/* Restore BARs and memory decoding without starting NVIDIA. Keep its
		 * suspend snapshot marked valid so the unlock transaction preserves
		 * it across its own pci_save_state(), including its BME-off image. */
		saved = pdev->state_saved;
		pci_restore_state(pdev);
		pdev->state_saved = saved;
		/* MSI-X masking accesses its BAR table, so power and memory decoding
		 * must be restored before disabling the IRQs. */
		runtime_irqs(pdev, false);
		ret = pm->resume(pm->data);
		runtime_irqs(pdev, true);
	}
	memalloc_noio_restore(flags);
	/* PCI still performs its normal fixups, wake configuration and NVIDIA
	 * callback. A failed unlock must not hand an uncontained GPU to NVIDIA. */
	return ret ? ret : pm->pci_resume(dev);
}

struct gu_runtime_pm *gu_runtime_pm_add(struct pci_dev *pdev,
				       int (*resume)(void *), void *data)
{
	struct device *dev = &pdev->dev;
	struct gu_runtime_pm *pm;
	int ret = 0;

	pm = kzalloc(sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return ERR_PTR(-ENOMEM);
	pci_dev_lock(pdev);
	/* Do not replace an ACPI/GenPD owner or interfere with Tegra's domain
	 * setup. The PCI bus callbacks remain responsible for every PM phase. */
	if (dev->driver || dev->pm_domain || dev->of_node || pdev->subordinate ||
	    !dev->bus || !dev->bus->pm || !dev->bus->pm->runtime_resume) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	pm->domain.ops = *dev->bus->pm;
	pm->domain.ops.runtime_resume = gu_runtime_resume;
	pm->pdev = pdev;
	pm->pci_resume = dev->bus->pm->runtime_resume;
	pm->resume = resume;
	pm->data = data;
	pm_runtime_disable(dev);
	dev_pm_domain_set(dev, &pm->domain);
	pm_runtime_enable(dev);
out:
	pci_dev_unlock(pdev);
	if (ret) {
		kfree(pm);
		return ERR_PTR(ret);
	}
	return pm;
}

void gu_runtime_pm_remove(struct gu_runtime_pm *pm)
{
	struct device *dev;

	if (!pm)
		return;
	dev = &pm->pdev->dev;
	pci_dev_lock(pm->pdev);
	/* Callbacks never take this lock, including a pending resume drained by
	 * pm_runtime_disable(). No callback may retain pm or data after this. */
	pm_runtime_disable(dev);
	if (dev->pm_domain == &pm->domain)
		dev_pm_domain_set(dev, NULL);
	pm_runtime_enable(dev);
	pci_dev_unlock(pm->pdev);
	kfree(pm);
}
