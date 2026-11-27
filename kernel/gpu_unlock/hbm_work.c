// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/timekeeping.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "hbm.h"
#include "registers.h"

struct gu_hbm_worker {
	struct pci_dev *pdev;
	void __iomem *bar;
	struct gu_hbm hbm;
	struct delayed_work work;
	struct notifier_block pci_nb, pm_nb;
	bool bound, paused, stopping, pci_registered, pm_registered;
	bool boot_seen;
	const char *last_reason;
	int last_error;
	int last_access;
	u64 last_report, last_status;
};

static bool nvidia(struct pci_dev *pdev)
{
	return pdev->driver && !strcmp(pdev->driver->name, "nvidia");
}

static void queue(struct gu_hbm_worker *worker, unsigned long delay)
{
	if (READ_ONCE(worker->bound) && !READ_ONCE(worker->paused) &&
	    !READ_ONCE(worker->stopping))
		mod_delayed_work(system_freezable_wq, &worker->work, delay);
}

static int active(struct pci_dev *pdev)
{
#ifdef GU_PM_ACTIVE_HAS_IGNORE
	return pm_runtime_get_if_active(&pdev->dev, true);
#else
	return pm_runtime_get_if_active(&pdev->dev);
#endif
}

/* The caller holds the PCI device lock. Never wake a runtime-suspended GPU. */
static int begin(struct gu_hbm_worker *worker)
{
	struct pci_dev *pdev = worker->pdev;
	u16 command;
	int pm;

	if (!nvidia(pdev) || pdev->current_state != PCI_D0)
		return -ENODEV;
	pm = active(pdev);
	if (!pm)
		return -EAGAIN;
	if (pci_read_config_word(pdev, PCI_COMMAND, &command) ||
	    command == 0xffff || !(command & PCI_COMMAND_MEMORY)) {
		if (pm > 0)
			pm_runtime_put(&pdev->dev);
		return -EAGAIN;
	}
	/* A disabled runtime-PM implementation needs no usage reference. */
	return pm > 0;
}

static void end(struct gu_hbm_worker *worker, int pm)
{
	if (pm > 0)
		pm_runtime_put(&worker->pdev->dev);
}

static const char *mode_name(enum gu_hbm_mode mode)
{
	static const char * const names[] = { "auto", "busy", "idle" };

	return names[mode];
}

static void sample(struct gu_hbm_worker *worker)
{
	struct gu_hbm *hbm = &worker->hbm;
	bool idle = hbm->idle, profile = hbm->profile, baseline = hbm->baseline;
	u64 start = ktime_get_ns(), elapsed;
	int ret;

	if (gu_hbm_booted(worker->bar)) {
		if (!worker->boot_seen)
			pci_dbg(worker->pdev, "gpu_unlock: HBM firmware booted gfw=%#x gsp=%#x\n",
				 readl(worker->bar + GU_GFW_BOOT_OFFSET),
				 readl(worker->bar + GU_GSP_RISCV_STATUS_OFFSET));
		worker->boot_seen = true;
	} else {
		worker->boot_seen = false;
	}
	ret = gu_hbm_poll(hbm, worker->bar, start);
	elapsed = ktime_get_ns() - start;
	if (!profile && hbm->profile) {
		u32 coeff = hbm->busy_coeff;
		u32 mhz = 27 * ((coeff >> 8) & 0xff) / (coeff & 0xff) / ((coeff >> 16) & 0x3f);

		pci_dbg(worker->pdev, "gpu_unlock: HBM busy profile %u MHz coeff=%#x idle_coeff=%#x fbpas=%#x first=%#x refresh=%#x\n",
			 mhz, coeff, hbm->idle_coeff, hbm->fbpas, hbm->first_fbpa,
			 readl(worker->bar + hbm->first_fbpa + 0x2a0));
	}
	if (idle != hbm->idle) {
		u32 coeff = readl(worker->bar + hbm->first_fbpa + 0x3c98);

		pci_dbg(worker->pdev, "gpu_unlock: HBM %s -> %s coeff=%#x mdiv=%u ndiv=%u pldiv=%u pll=%#x refresh=%#x fb_req=%#x fb_ack=%#x active_counters=%#x elapsed_us=%llu\n",
			 idle ? "idle" : "busy", hbm->idle ? "idle" : "busy",
			 coeff, coeff & 0xff, (coeff >> 8) & 0xff, (coeff >> 16) & 0x3f,
			 readl(worker->bar + hbm->first_fbpa + 0x3c90),
			 readl(worker->bar + hbm->first_fbpa + 0x2a0),
			 readl(worker->bar + 0x10a7c0), readl(worker->bar + 0x10a7c4),
			 hbm->active_counters,
			 (unsigned long long)(elapsed / 1000));
	}
	if (!baseline && hbm->baseline) {
		unsigned int i;

		for (i = 0; i < 16; i++) {
			const struct gu_hbm_counter *c = &hbm->counter[i];

			if (hbm->counters & (1u << i))
				pci_dbg(worker->pdev, "gpu_unlock: HBM counter %u masks=%#x/%#x/%#x control=%#x threshold=%#x count=%#x\n",
					 i, c->mask[0], c->mask[1], c->mask[2], c->control, c->threshold, c->count);
		}
	}
	if (ret) {
		if (ret != worker->last_error || hbm->reason != worker->last_reason ||
		    (ret != -EAGAIN && start - worker->last_report >= 5000000000ull)) {
			if (ret == -EAGAIN)
				pci_dbg(worker->pdev, "gpu_unlock: HBM %s (%d) reg=%#x value=%#x\n",
					 hbm->reason ? hbm->reason : "hardware unavailable", ret,
					 hbm->reg, hbm->value);
			else
				pci_warn(worker->pdev, "gpu_unlock: HBM %s (%d) reg=%#x value=%#x busy_coeff=%#x idle_coeff=%#x coverage=%#x/%#x/%#x idle=%u fb_stopped=%u\n",
					 hbm->reason ? hbm->reason : "hardware unavailable", ret,
					 hbm->reg, hbm->value, hbm->busy_coeff, hbm->idle_coeff,
					 hbm->coverage[0], hbm->coverage[1], hbm->coverage[2], hbm->idle, hbm->fb_stopped);
			worker->last_report = start;
		}
	} else if (worker->last_error || !worker->last_report) {
		pci_dbg(worker->pdev, "gpu_unlock: HBM activity sampling active counters=%#x\n", hbm->counters);
		worker->last_report = start;
	}
	worker->last_error = ret;
	worker->last_reason = hbm->reason;
	if (start - worker->last_status >= 5000000000ull) {
		pci_dbg(worker->pdev, "gpu_unlock: HBM status profile=%u baseline=%u idle=%u quiet_ms=%llu active_counters=%#x\n",
			hbm->profile, hbm->baseline, hbm->idle,
			(unsigned long long)(hbm->baseline ? (start - hbm->quiet_since) / 1000000 : 0),
			hbm->active_counters);
		worker->last_status = start;
	}
}

static void poll(struct work_struct *work)
{
	struct gu_hbm_worker *worker = container_of(work, struct gu_hbm_worker, work.work);
	int pm;

	if (READ_ONCE(worker->paused) || READ_ONCE(worker->stopping) ||
	    !READ_ONCE(worker->bound))
		return;
	if (device_trylock(&worker->pdev->dev)) {
		pm = begin(worker);
		if (pm >= 0) {
			if (worker->last_access)
				pci_dbg(worker->pdev, "gpu_unlock: HBM PCI access resumed\n");
			sample(worker);
			end(worker, pm);
		} else {
			if (pm != worker->last_access)
				pci_dbg(worker->pdev, "gpu_unlock: HBM waiting for active PCI device (%d) state=%d\n",
					 pm, worker->pdev->current_state);
			gu_hbm_reset(&worker->hbm);
		}
		worker->last_access = pm < 0 ? pm : 0;
		device_unlock(&worker->pdev->dev);
	}
	queue(worker, msecs_to_jiffies(GU_HBM_POLL_MS));
}

static void restore_locked(struct gu_hbm_worker *worker)
{
	int pm = begin(worker);

	if (pm >= 0) {
		bool idle = worker->hbm.idle;
		int ret = gu_hbm_restore(&worker->hbm, worker->bar);

		if (ret)
			pci_warn(worker->pdev, "gpu_unlock: HBM busy restore deferred (%d) %s reg=%#x value=%#x\n",
				 ret, worker->hbm.reason ? worker->hbm.reason : "hardware unavailable",
				 worker->hbm.reg, worker->hbm.value);
		else if (idle)
			pci_dbg(worker->pdev, "gpu_unlock: HBM busy profile restored for detach/sleep\n");
		end(worker, pm);
	} else if (worker->hbm.idle) {
		pci_warn(worker->pdev, "gpu_unlock: HBM busy restore deferred: PCI device inactive (%d)\n", pm);
	}
	gu_hbm_reset(&worker->hbm);
	worker->boot_seen = false;
}

static int pci_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	struct gu_hbm_worker *worker = container_of(nb, struct gu_hbm_worker, pci_nb);

	if (data != &worker->pdev->dev)
		return NOTIFY_DONE;
	/* Bus notifications run with the PCI device lock held. Work only uses
	 * trylock, so cancelling it here cannot wait on that lock. */
	if (event == BUS_NOTIFY_BOUND_DRIVER && nvidia(worker->pdev)) {
		WRITE_ONCE(worker->bound, true);
		queue(worker, 0);
	} else if (event == BUS_NOTIFY_UNBIND_DRIVER) {
		WRITE_ONCE(worker->bound, false);
		cancel_delayed_work_sync(&worker->work);
		restore_locked(worker);
		pci_dbg(worker->pdev, "gpu_unlock: HBM governor stopped for driver unbind\n");
	}
	return NOTIFY_DONE;
}

static int pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	struct gu_hbm_worker *worker = container_of(nb, struct gu_hbm_worker, pm_nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		WRITE_ONCE(worker->paused, true);
		cancel_delayed_work_sync(&worker->work);
		pci_dbg(worker->pdev, "gpu_unlock: HBM governor paused for sleep\n");
		pci_dev_lock(worker->pdev);
		restore_locked(worker);
		pci_dev_unlock(worker->pdev);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		pci_dev_lock(worker->pdev);
		gu_hbm_reset(&worker->hbm);
		worker->boot_seen = false;
		pci_dev_unlock(worker->pdev);
		WRITE_ONCE(worker->paused, false);
		pci_dbg(worker->pdev, "gpu_unlock: HBM governor waiting for firmware after sleep\n");
		queue(worker, 0);
		break;
	}
	return NOTIFY_DONE;
}

struct gu_hbm_worker *gu_hbm_start(struct pci_dev *pdev)
{
	struct gu_hbm_worker *worker;
	int ret;

	worker = kzalloc(sizeof(*worker), GFP_KERNEL);
	if (!worker)
		return ERR_PTR(-ENOMEM);
	gu_hbm_init(&worker->hbm);
	worker->pdev = pdev;
	INIT_DELAYED_WORK(&worker->work, poll);
	worker->pci_nb.notifier_call = pci_notify;
	worker->pm_nb.notifier_call = pm_notify;
	/* Quiesce at the start of Linux's sleep notification chain. This does
	 * not depend on any particular GPU driver's notifier priority. */
	worker->pm_nb.priority = INT_MAX;
	if (pci_resource_len(pdev, 0) < 0xa00000 ||
	    !(worker->bar = pci_iomap_range(pdev, 0, 0, 0xa00000))) {
		ret = -ENODEV;
		goto fail;
	}
	ret = bus_register_notifier(&pci_bus_type, &worker->pci_nb);
	if (ret)
		goto fail;
	worker->pci_registered = true;
	ret = register_pm_notifier(&worker->pm_nb);
	if (ret)
		goto fail;
	worker->pm_registered = true;
	pci_info(pdev, "gpu_unlock: HBM governor registered mode=%s idle_ms=%llu poll_ms=%u idle_ndiv=%u idle_pldiv=%u idle_refresh=%u transition=fb-stop/bypass\n",
		 mode_name(worker->hbm.policy.mode),
		 (unsigned long long)(GU_HBM_IDLE_NS / 1000000), GU_HBM_POLL_MS,
		 gu_hbm_idle_ndiv(), GU_HBM_IDLE_PLDIV, GU_HBM_IDLE_REFRESH);
	pci_dev_lock(pdev);
	WRITE_ONCE(worker->bound, nvidia(pdev));
	pci_dev_unlock(pdev);
	queue(worker, 0);
	return worker;
fail:
	gu_hbm_remove(worker);
	return ERR_PTR(ret);
}

void gu_hbm_remove(struct gu_hbm_worker *worker)
{
	if (!worker)
		return;
	WRITE_ONCE(worker->stopping, true);
	if (worker->pm_registered)
		unregister_pm_notifier(&worker->pm_nb);
	if (worker->pci_registered)
		bus_unregister_notifier(&pci_bus_type, &worker->pci_nb);
	cancel_delayed_work_sync(&worker->work);
	if (worker->bar) {
		pci_dev_lock(worker->pdev);
		restore_locked(worker);
		pci_dev_unlock(worker->pdev);
		pci_iounmap(worker->pdev, worker->bar);
	}
	kfree(worker);
}
