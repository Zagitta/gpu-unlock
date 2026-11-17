// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only kernel boundaries are simulated; the callbacks under test come from
 * pm.c. Assertions check ordering, IRQ balance and callback ownership. */
#define GFP_KERNEL 0
#define PCI_D0 0
#define PCI_MSIX_FLAGS_QSIZE 2047
#define container_of(p, t, member) ((t *)((char *)(p) - offsetof(t, member)))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)

struct device;
struct dev_pm_ops {
	int (*resume_noirq)(struct device *);
	int (*runtime_suspend)(struct device *);
	int (*runtime_resume)(struct device *);
};
struct dev_pm_domain { struct dev_pm_ops ops; };
struct device_driver { const char *name; };
struct bus_type { const struct dev_pm_ops *pm; };
struct device_type {
	const char *name;
	const struct dev_pm_ops *pm;
	void (*release)(struct device *);
};
struct device {
	struct device *parent;
	const struct device_type *type;
	struct device_driver *driver;
	struct bus_type *bus;
	struct dev_pm_domain *pm_domain;
	void *of_node;
	struct { bool irq_safe; int disable_depth; } power;
	bool locked;
};
struct msi_desc { unsigned int irq, nvec_used; struct msi_desc *next; };
struct pci_dev {
	struct device dev;
	void *subordinate;
	bool state_saved, msi_enabled, msix_enabled;
	unsigned int irq;
	struct msi_desc *msi;
	unsigned int vectors[2048];
};
struct completion { bool done; };

static int allocations, noio, irq_depth[4096];
static int power_error, unlock_error, pci_error;
static int powers, restores, unlocks, resumes;
static bool drain_pending;

static void *kzalloc(size_t size, int flags)
{
	(void)flags;
	allocations++;
	return calloc(1, size);
}
static void kfree(void *p) { if (p) allocations--; free(p); }
static void init_completion(struct completion *c) { c->done = false; }
static void complete(struct completion *c) { c->done = true; }
static void wait_for_completion(struct completion *c) { assert(c->done); }
static void device_initialize(struct device *d) { (void)d; }
static void device_enable_async_suspend(struct device *d) { (void)d; }
static int dev_set_name(struct device *d, const char *n) { (void)d; (void)n; return 0; }
static int device_add(struct device *d) { (void)d; return 0; }
static void put_device(struct device *d) { d->type->release(d); }
static void device_unregister(struct device *d) { put_device(d); }
static void pci_dev_lock(struct pci_dev *p) { assert(!p->dev.locked); p->dev.locked = true; }
static void pci_dev_unlock(struct pci_dev *p) { assert(p->dev.locked); p->dev.locked = false; }
static unsigned int memalloc_noio_save(void) { unsigned int old = noio; noio++; return old; }
static void memalloc_noio_restore(unsigned int old) { noio = old; }
static void disable_irq(unsigned int irq) { assert(irq < 4096); irq_depth[irq]++; }
static void enable_irq(unsigned int irq) { assert(irq_depth[irq] > 0); irq_depth[irq]--; }
#ifdef GU_MSI_GET_VIRQ
static unsigned int msi_get_virq(struct device *d, unsigned int i)
{
	assert(i < 2048);
	return container_of(d, struct pci_dev, dev)->vectors[i];
}
#endif
#define for_each_pci_msi_entry(desc, pdev) \
	for ((desc) = (pdev)->msi; (desc); (desc) = (desc)->next)

static void pm_runtime_disable(struct device *d)
{
	if (drain_pending) {
		drain_pending = false;
		assert(d->locked && d->pm_domain);
		assert(d->pm_domain->ops.runtime_resume(d) == 0);
	}
	d->power.disable_depth++;
}
static void pm_runtime_enable(struct device *d)
{
	assert(d->power.disable_depth > 0);
	d->power.disable_depth--;
}
static void dev_pm_domain_set(struct device *d, struct dev_pm_domain *pd)
{
	assert(d->locked && d->power.disable_depth > 0);
	assert(!pd || !d->driver);
	d->pm_domain = pd;
}
static void check_irqs(struct pci_dev *p, int expected)
{
#ifdef CONFIG_PCI_MSI
	if (p->msi_enabled || p->msix_enabled) {
		struct msi_desc *m;
		unsigned int i;

		for_each_pci_msi_entry(m, p)
			for (i = 0; i < m->nvec_used; i++)
				assert(irq_depth[m->irq + i] == expected);
		return;
	}
#endif
	if (p->irq)
		assert(irq_depth[p->irq] == expected);
}
static int pci_set_power_state(struct pci_dev *p, int state)
{
	assert(state == PCI_D0 && noio == 1);
	check_irqs(p, 0);
	powers++;
	return power_error;
}
static void pci_restore_state(struct pci_dev *p)
{
	assert(powers == 1 && noio == 1);
	check_irqs(p, 0);
	p->state_saved = false;
	restores++;
}
static int unlock(void *data)
{
	struct pci_dev *p = data;

	assert(powers == 1 && restores == 1 && resumes == 0);
	assert(p->state_saved && noio == 1);
	check_irqs(p, 1);
	unlocks++;
	return unlock_error;
}
static int pci_resume(struct device *d)
{
	struct pci_dev *p = container_of(d, struct pci_dev, dev);

	assert(noio == 0);
	check_irqs(p, 0);
	if (d->driver && !strcmp(d->driver->name, "nvidia"))
		assert(unlocks == 1 && p->state_saved);
	resumes++;
	return pci_error;
}
static int noirq(struct device *d) { (void)d; return 0; }
static const struct dev_pm_ops pci_ops = {
	.resume_noirq = noirq, .runtime_suspend = noirq, .runtime_resume = pci_resume,
};
static struct bus_type pci_bus = { .pm = &pci_ops };
static struct device_driver nvidia = { .name = "nvidia" }, vfio = { .name = "vfio-pci" };

#include "../../kernel/gpu_unlock/pm.c"

static void reset_counts(void)
{
	powers = restores = unlocks = resumes = 0;
	power_error = unlock_error = pci_error = 0;
}

int main(void)
{
	struct pci_dev p = { .dev = { .bus = &pci_bus }, .irq = 33, .state_saved = true };
	struct gu_runtime_pm *pm = gu_runtime_pm_add(&p, unlock, &p);
	struct gu_runtime_pm *other;

	assert(!IS_ERR(pm) && p.dev.pm_domain == &pm->domain);
	assert(pm->domain.ops.resume_noirq == pci_ops.resume_noirq);
	assert(pm->domain.ops.runtime_suspend == pci_ops.runtime_suspend);
	assert(!p.dev.power.disable_depth && allocations == 1);
	other = gu_runtime_pm_add(&p, unlock, &p);
	assert(PTR_ERR(other) == -EOPNOTSUPP && allocations == 1);
	assert(p.dev.pm_domain == &pm->domain);
	p.dev.driver = &nvidia;
	/* Driver removal and system sleep can request runtime resume while they
	 * hold the PCI device lock. Recursively taking it would deadlock. */
	pci_dev_lock(&p);
	assert(pm->domain.ops.runtime_resume(&p.dev) == 0);
	pci_dev_unlock(&p);
	assert(powers == 1 && restores == 1 && unlocks == 1 && resumes == 1);

	reset_counts();
	unlock_error = -EIO;
	assert(pm->domain.ops.runtime_resume(&p.dev) == -EIO);
	assert(unlocks == 1 && resumes == 0 && noio == 0);
	check_irqs(&p, 0);
	reset_counts();
	power_error = -ENODEV;
	assert(pm->domain.ops.runtime_resume(&p.dev) == -ENODEV);
	assert(powers == 1 && restores == 0 && unlocks == 0 && resumes == 0 && noio == 0);
	check_irqs(&p, 0);
	reset_counts();
	pci_error = -EIO;
	assert(pm->domain.ops.runtime_resume(&p.dev) == -EIO && resumes == 1);
	reset_counts();
	p.dev.power.irq_safe = true;
	assert(pm->domain.ops.runtime_resume(&p.dev) == -EOPNOTSUPP);
	assert(powers == 0 && resumes == 0);
	p.dev.power.irq_safe = false;

	reset_counts();
	p.dev.driver = &vfio;
	assert(pm->domain.ops.runtime_resume(&p.dev) == 0 && resumes == 1 && powers == 0);
	p.dev.driver = NULL;
	assert(pm->domain.ops.runtime_resume(&p.dev) == 0 && resumes == 2);
	p.dev.driver = &nvidia;
#ifdef CONFIG_PCI_MSI
	{
		struct msi_desc second = { .irq = 99, .nvec_used = 1 };
		struct msi_desc first = { .irq = 60, .nvec_used = 2, .next = &second };

		p.msix_enabled = true;
		p.msi = &first;
		p.vectors[0] = 60;
		p.vectors[1] = 61;
		p.vectors[127] = 99;
		reset_counts();
		assert(pm->domain.ops.runtime_resume(&p.dev) == 0 && resumes == 1);
		check_irqs(&p, 0);
		p.msix_enabled = false;
		p.msi = NULL;
	}
#endif
	reset_counts();
	drain_pending = true;
	gu_runtime_pm_remove(pm);
	assert(!drain_pending && resumes == 1 && !p.dev.pm_domain && allocations == 0);
	assert(!p.dev.locked && !p.dev.power.disable_depth);
	check_irqs(&p, 0);
	other = gu_runtime_pm_add(&p, unlock, &p);
	assert(PTR_ERR(other) == -EOPNOTSUPP && allocations == 0);
	p.dev.driver = NULL;
	p.dev.of_node = &p;
	assert(PTR_ERR(gu_runtime_pm_add(&p, unlock, &p)) == -EOPNOTSUPP);
	assert(!allocations && !p.dev.pm_domain);
	puts("runtime PM ordering, IRQs, error paths and removal passed");
	return 0;
}
