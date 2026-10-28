// SPDX-License-Identifier: GPL-2.0-only
#include <linux/async.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>

#include "unlock_610.h"
#include "recipes.h"
#include "registers.h"

static char *targets;
module_param(targets, charp, 0444);
MODULE_PARM_DESC(targets, "\"all\" or comma-separated PCI addresses of supported cards to unlock");

static bool resume = true;
module_param(resume, bool, 0644);
MODULE_PARM_DESC(resume, "Unlock the cards again after resume from system suspend (default: on)");

static bool runtime_unlock = true;
module_param(runtime_unlock, bool, 0644);
MODULE_PARM_DESC(runtime_unlock, "Unlock the cards again after PCIe runtime PM D3cold wakeup and hotplug rescan (default: on)");

static unsigned int unlock_count;
module_param(unlock_count, uint, 0444);
MODULE_PARM_DESC(unlock_count, "Total number of hardware unlock transactions completed since module load");

static unsigned int last_ss0;
module_param(last_ss0, uint, 0444);
MODULE_PARM_DESC(last_ss0, "Last verified FEAT_SS0 register value");

static unsigned int last_ss1;
module_param(last_ss1, uint, 0444);
MODULE_PARM_DESC(last_ss1, "Last verified FEAT_SS1 register value");

static int last_error;
module_param(last_error, int, 0444);
MODULE_PARM_DESC(last_error, "Result of the most recent unlock transaction (0 on success)");

/* A target card tracked across boot, driver bind/unbind, runtime PM D3cold
 * transitions, system suspend/resume, and PCI remove/rescan cycles.
 */
struct card {
	struct list_head node;
	struct kref ref;
	struct mutex lock;
	struct gu_device device;
	bool unlocked;
	bool driver_bound;
};

static LIST_HEAD(cards);
static DEFINE_MUTEX(cards_lock);
static DEFINE_MUTEX(shared_lock);
static DEFINE_MUTEX(pm_hook_lock);
static ASYNC_DOMAIN_EXCLUSIVE(unlock_domain);
static struct gpu_unlock_610_shared shared;
static bool notifiers_registered;

static struct device_driver *hooked_drv;
static const struct dev_pm_ops *orig_pm_ops;
static struct dev_pm_ops gu_pm_ops;

static void release_card(struct kref *ref)
{
	struct card *card = container_of(ref, struct card, ref);

	pci_dev_put(card->device.pdev);
	kfree(card);
}

static void put_card(struct card *card)
{
	if (card)
		kref_put(&card->ref, release_card);
}

static struct card *find_card_get(struct pci_dev *pdev)
{
	struct card *card;

	mutex_lock(&cards_lock);
	list_for_each_entry(card, &cards, node) {
		if (card->device.pdev == pdev) {
			kref_get(&card->ref);
			mutex_unlock(&cards_lock);
			return card;
		}
	}
	mutex_unlock(&cards_lock);
	return NULL;
}

static bool any_card_bound(void)
{
	struct card *card;
	bool bound = false;

	mutex_lock(&cards_lock);
	list_for_each_entry(card, &cards, node) {
		if (READ_ONCE(card->driver_bound)) {
			bound = true;
			break;
		}
	}
	mutex_unlock(&cards_lock);
	return bound;
}

/* True when the comma-separated targets list names this PCI device. */
static bool listed(const char *name)
{
	const char *entry, *end;

	for (entry = targets; *entry; entry = *end ? end + 1 : end) {
		end = strchrnul(entry, ',');
		if (strlen(name) == end - entry && !strncasecmp(name, entry, end - entry))
			return true;
	}
	return false;
}

static bool match_target(struct pci_dev *pdev, const struct gu_card **recipe)
{
	if (!targets || !*targets || pdev->vendor != PCI_VENDOR_ID_NVIDIA)
		return false;
	*recipe = gu_card_find(pdev->device);
	if (!*recipe)
		return false;
	return !strcmp(targets, "all") || listed(pci_name(pdev));
}

static struct card *add_card(struct pci_dev *pdev, const struct gu_card *recipe)
{
	struct card *card, *existing;

	card = kzalloc(sizeof(*card), GFP_KERNEL);
	if (!card) {
		pci_err(pdev, "gpu_unlock: card allocation failed\n");
		return NULL;
	}
	kref_init(&card->ref);
	mutex_init(&card->lock);
	card->device.pdev = pci_dev_get(pdev);
	card->device.recipe = recipe;

	mutex_lock(&cards_lock);
	list_for_each_entry(existing, &cards, node) {
		if (existing->device.pdev == pdev) {
			kref_get(&existing->ref);
			mutex_unlock(&cards_lock);
			put_card(card);
			return existing;
		}
	}
	kref_get(&card->ref);
	list_add_tail(&card->node, &cards);
	mutex_unlock(&cards_lock);
	return card;
}

static void remove_card_dev(struct pci_dev *pdev)
{
	struct card *card, *found = NULL;

	mutex_lock(&cards_lock);
	list_for_each_entry(card, &cards, node) {
		if (card->device.pdev == pdev) {
			list_del_init(&card->node);
			found = card;
			break;
		}
	}
	mutex_unlock(&cards_lock);
	if (found) {
		WRITE_ONCE(found->driver_bound, false);
		put_card(found);
	}
}

/* Load and cache the family firmware bundle in RAM across the module lifetime.
 * If an earlier attempt failed (e.g. before rootfs mount), reset the failed
 * slot so subsequent bind or runtime-resume hooks retry cleanly.
 */
static int ensure_shared(const struct gu_device *dev)
{
	struct gpu_unlock_610_firmware *slot;
	int ret;

	mutex_lock(&shared_lock);
	slot = (struct gpu_unlock_610_firmware *)
		gpu_unlock_610_bundle_find(&shared, dev->recipe->family->fw);
	if (slot && slot->prepared && slot->error)
		memset(slot, 0, sizeof(*slot));
	ret = gpu_unlock_610_load_shared(dev, &shared);
	mutex_unlock(&shared_lock);
	if (ret)
		pci_err(dev->pdev, "gpu_unlock: firmware load failed (%d)\n", ret);
	return ret;
}

static void record_unlock_result(int ret)
{
	if (!ret) {
		WRITE_ONCE(unlock_count, READ_ONCE(unlock_count) + 1);
		WRITE_ONCE(last_ss0, GU_SS0_UNLOCK);
		WRITE_ONCE(last_ss1, GU_SS1_UNLOCK);
		WRITE_ONCE(last_error, 0);
	} else if (ret == -EALREADY) {
		WRITE_ONCE(last_ss0, GU_SS0_UNLOCK);
		WRITE_ONCE(last_ss1, GU_SS1_UNLOCK);
	} else {
		WRITE_ONCE(last_error, ret);
	}
}

/* Caller holds card->lock (and pci_dev_lock(pdev) when required by context). */
static int unlock_card(struct card *card, bool is_resume, const char *phase)
{
	struct pci_dev *pdev = card->device.pdev;
	int ret;

	ret = ensure_shared(&card->device);
	if (ret)
		return ret;
	ret = gpu_unlock_610_unlock(&card->device, &shared, is_resume);
	record_unlock_result(ret);
	if (!ret) {
		card->unlocked = true;
		pci_info(pdev, "gpu_unlock: unlocked (%s)\n", phase);
	} else if (ret == -EALREADY) {
		card->unlocked = true;
		pci_dbg(pdev, "gpu_unlock: already unlocked (%s)\n", phase);
		ret = 0;
	} else {
		pci_err(pdev, "gpu_unlock: unlock failed on %s (%d)\n", phase, ret);
	}
	return ret;
}

/* The next supported NVIDIA card after from, like pci_get_device(). */
static struct pci_dev *next_supported(struct pci_dev *from, const struct gu_card **recipe)
{
	struct pci_dev *pdev = from;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev)))
		if ((*recipe = gu_card_find(pdev->device)))
			return pdev;
	return NULL;
}

/* At boot, before nvidia binds the card. Runs concurrently for each card. */
static void unlock_one(void *data, async_cookie_t cookie)
{
	struct card *card = data;
	struct pci_dev *pdev = card->device.pdev;

	pci_dev_lock(pdev);
	mutex_lock(&card->lock);
	if (pdev->driver)
		pci_err(pdev, "gpu_unlock: unlock failed (%d)\n", -EBUSY);
	else
		unlock_card(card, false, "boot");
	mutex_unlock(&card->lock);
	pci_dev_unlock(pdev);
	put_card(card);
}

/* After system resume, before nvidia's PM notifier resumes the card. Cards
 * that remained runtime-suspended in D3cold across system sleep are skipped
 * here and unlocked by gu_pm_runtime_resume() when they actually wake to D0.
 */
static void unlock_again(void *data, async_cookie_t cookie)
{
	struct card *card = data;
	struct pci_dev *pdev = card->device.pdev;
	bool nvidia;

	if (pm_runtime_status_suspended(&pdev->dev) || pdev->current_state != PCI_D0) {
		pci_dbg(pdev, "gpu_unlock: still in D3cold after system resume, deferring to runtime resume\n");
		put_card(card);
		return;
	}
	pci_dev_lock(pdev);
	mutex_lock(&card->lock);
	nvidia = pdev->driver && !strcmp(pdev->driver->name, "nvidia");
	if (nvidia)
		unlock_card(card, true, "resume");
	else
		pci_info(pdev, "gpu_unlock: not bound to nvidia, left as is after resume\n");
	mutex_unlock(&card->lock);
	pci_dev_unlock(pdev);
	put_card(card);
}

/* Prepare all selected firmware families, then run the ready cards concurrently. */
static void unlock_cards(async_func_t fn)
{
	struct card *card;

	mutex_lock(&cards_lock);
	list_for_each_entry(card, &cards, node) {
		if (!ensure_shared(&card->device)) {
			kref_get(&card->ref);
			async_schedule_domain(fn, card, &unlock_domain);
		}
	}
	mutex_unlock(&cards_lock);
	async_synchronize_full_domain(&unlock_domain);
}

/* Intercepts pci_pm_runtime_resume() right after pci_restore_standard_config()
 * transitions the card from D3cold -> PCI_D0 and restores BARs/PCI_COMMAND,
 * and right before nv_pmops_runtime_resume() boots GSP-RM.
 */
static int gu_pm_runtime_resume(struct device *dev)
{
	const struct dev_pm_ops *ops = READ_ONCE(orig_pm_ops);
	struct pci_dev *pdev = dev_is_pci(dev) ? to_pci_dev(dev) : NULL;
	struct card *card;

	if (runtime_unlock && pdev && pci_get_drvdata(pdev)) {
		card = find_card_get(pdev);
		if (card) {
			if (READ_ONCE(card->driver_bound)) {
				mutex_lock(&card->lock);
				unlock_card(card, true, "runtime resume");
				mutex_unlock(&card->lock);
			}
			put_card(card);
		}
	}
	return ops && ops->runtime_resume ? ops->runtime_resume(dev) : 0;
}

/* Intercepts dpm_resume() (where device_resume() already holds device_lock(dev)
 * and pci_pm_resume() has restored PCI_D0) before nv_pmops_resume() runs.
 */
static void gu_pm_system_resume_common(struct device *dev, const char *phase)
{
	struct pci_dev *pdev = dev_is_pci(dev) ? to_pci_dev(dev) : NULL;
	struct card *card;

	if (!resume || !pdev || !pci_get_drvdata(pdev) ||
	    pm_runtime_status_suspended(dev) || pdev->current_state != PCI_D0)
		return;
	card = find_card_get(pdev);
	if (card) {
		if (READ_ONCE(card->driver_bound)) {
			mutex_lock(&card->lock);
			unlock_card(card, true, phase);
			mutex_unlock(&card->lock);
		}
		put_card(card);
	}
}

static int gu_pm_resume(struct device *dev)
{
	const struct dev_pm_ops *ops = READ_ONCE(orig_pm_ops);

	gu_pm_system_resume_common(dev, "system resume");
	return ops && ops->resume ? ops->resume(dev) : 0;
}

static int gu_pm_restore(struct device *dev)
{
	const struct dev_pm_ops *ops = READ_ONCE(orig_pm_ops);

	gu_pm_system_resume_common(dev, "hibernate restore");
	return ops && ops->restore ? ops->restore(dev) : 0;
}

static int gu_pm_thaw(struct device *dev)
{
	const struct dev_pm_ops *ops = READ_ONCE(orig_pm_ops);

	gu_pm_system_resume_common(dev, "hibernate thaw");
	return ops && ops->thaw ? ops->thaw(dev) : 0;
}

static void install_pm_hook(struct device_driver *drv)
{
	if (!drv || strcmp(drv->name, "nvidia") != 0)
		return;
	mutex_lock(&pm_hook_lock);
	if (drv->pm && drv->pm != &gu_pm_ops) {
		orig_pm_ops = drv->pm;
		hooked_drv = drv;
		gu_pm_ops = *orig_pm_ops;
		gu_pm_ops.runtime_resume = gu_pm_runtime_resume;
		gu_pm_ops.resume = gu_pm_resume;
		gu_pm_ops.restore = gu_pm_restore;
		gu_pm_ops.thaw = gu_pm_thaw;
		WRITE_ONCE(drv->pm, &gu_pm_ops);
	}
	mutex_unlock(&pm_hook_lock);
}

static void remove_pm_hook(void)
{
	mutex_lock(&pm_hook_lock);
	if (hooked_drv && hooked_drv->pm == &gu_pm_ops)
		WRITE_ONCE(hooked_drv->pm, orig_pm_ops);
	hooked_drv = NULL;
	orig_pm_ops = NULL;
	mutex_unlock(&pm_hook_lock);
}

static int gpu_unlock_pci_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;
	struct pci_dev *pdev = dev_is_pci(dev) ? to_pci_dev(dev) : NULL;
	const struct gu_card *recipe = NULL;
	struct card *card;

	if (!pdev)
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		if (match_target(pdev, &recipe)) {
			card = add_card(pdev, recipe);
			put_card(card);
		}
		break;

	case BUS_NOTIFY_BIND_DRIVER:
		card = find_card_get(pdev);
		if (!card && match_target(pdev, &recipe))
			card = add_card(pdev, recipe);
		if (!card)
			break;
		if (dev->driver && !strcmp(dev->driver->name, "nvidia")) {
			install_pm_hook(dev->driver);
			if (runtime_unlock) {
				/* really_probe() caller already holds device_lock(dev). */
				mutex_lock(&card->lock);
				if (pdev->current_state != PCI_D0 &&
				    pdev->current_state != PCI_UNKNOWN)
					pci_set_power_state(pdev, PCI_D0);
				unlock_card(card, false, "bind");
				mutex_unlock(&card->lock);
			}
			WRITE_ONCE(card->driver_bound, true);
		}
		put_card(card);
		break;

	case BUS_NOTIFY_UNBIND_DRIVER:
		card = find_card_get(pdev);
		if (card) {
			/* Clear driver_bound before __device_release_driver() calls
			 * pm_runtime_get_sync(dev) for nv_pci_remove().
			 */
			WRITE_ONCE(card->driver_bound, false);
			put_card(card);
		}
		break;

	case BUS_NOTIFY_UNBOUND_DRIVER:
		if (!any_card_bound())
			remove_pm_hook();
		break;

	case BUS_NOTIFY_DEL_DEVICE:
		remove_card_dev(pdev);
		if (!any_card_bound())
			remove_pm_hook();
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block pci_nb = {
	.notifier_call = gpu_unlock_pci_notify,
};

/* nvidia resumes its cards from its own notifier, at the default priority 0,
 * once this returns: every card must be finished by then. */
static int gpu_unlock_pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	if (event == PM_POST_SUSPEND && resume)
		unlock_cards(unlock_again);
	return NOTIFY_DONE;
}

static struct notifier_block pm_nb = {
	.notifier_call = gpu_unlock_pm_notify,
	.priority = 1,
};

static void report_unmatched(void)
{
	const char *entry, *end;
	struct card *card;
	bool matched;

	mutex_lock(&cards_lock);
	for (entry = targets; *entry; entry = *end ? end + 1 : end) {
		end = strchrnul(entry, ',');
		matched = false;
		list_for_each_entry(card, &cards, node) {
			const char *name = pci_name(card->device.pdev);

			if (strlen(name) == end - entry && !strncasecmp(name, entry, end - entry)) {
				matched = true;
				break;
			}
		}
		if (!matched)
			pr_err("gpu_unlock: no supported card at \"%.*s\"\n",
			       (int)(end - entry), entry);
	}
	mutex_unlock(&cards_lock);
}

static int __init gpu_unlock_init(void)
{
	struct pci_dev *pdev = NULL;
	struct card *card;
	const struct gu_card *recipe;
	bool all, enumerated = true;

	/* Loaded ahead of nvidia on every boot; the command line is the opt-in. */
	if (!targets || !*targets)
		return 0;
	all = !strcmp(targets, "all");
	while ((pdev = next_supported(pdev, &recipe))) {
		if (!all && !listed(pci_name(pdev)))
			continue;
		card = add_card(pdev, recipe);
		if (!card) {
			enumerated = false;
			pci_dev_put(pdev);
			break;
		}
		if (pdev->dev.driver && !strcmp(pdev->dev.driver->name, "nvidia")) {
			install_pm_hook(pdev->dev.driver);
			WRITE_ONCE(card->driver_bound, true);
		}
		put_card(card);
	}
	if (!all && enumerated)
		report_unmatched();
	bus_register_notifier(&pci_bus_type, &pci_nb);
	register_pm_notifier(&pm_nb);
	notifiers_registered = true;
	/* nvidia loads once this returns: every card must be finished by then. */
	unlock_cards(unlock_one);
	return 0;
}

static void __exit gpu_unlock_exit(void)
{
	struct card *card, *next;
	LIST_HEAD(removed);

	if (notifiers_registered) {
		unregister_pm_notifier(&pm_nb);
		bus_unregister_notifier(&pci_bus_type, &pci_nb);
	}
	async_synchronize_full_domain(&unlock_domain);
	remove_pm_hook();
	mutex_lock(&cards_lock);
	list_replace_init(&cards, &removed);
	mutex_unlock(&cards_lock);
	list_for_each_entry_safe(card, next, &removed, node) {
		list_del_init(&card->node);
		put_card(card);
	}
	mutex_lock(&shared_lock);
	gpu_unlock_610_release_shared(&shared);
	mutex_unlock(&shared_lock);
}

module_init(gpu_unlock_init);
module_exit(gpu_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ampere pre-driver hardware unlock");
MODULE_VERSION("2.0.0");
