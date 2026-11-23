// SPDX-License-Identifier: GPL-2.0-only
#include <linux/async.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "fuses.h"
#include "ecc.h"
#include "inforom.h"
#include "pcie.h"
#include "pm.h"
#include "registers.h"
#include "unlock.h"
#include "recipes.h"
#include "vram.h"
#include "hbm.h"
#include "overrides.h"

static char *targets;
module_param(targets, charp, 0444);
MODULE_PARM_DESC(targets, "\"all\" or comma-separated PCI addresses of supported Ampere cards to unlock");

static bool resume = true;
module_param(resume, bool, 0644);
MODULE_PARM_DESC(resume, "Unlock the cards again after resume from suspend, which powers them off (default: on)");

static bool runtime_resume;
module_param(runtime_resume, bool, 0444);
MODULE_PARM_DESC(runtime_resume, "Experimental: restore unlocks before NVIDIA runtime resume (default: off)");

static bool vram_temps = true;
module_param(vram_temps, bool, 0444);
MODULE_PARM_DESC(vram_temps, "Report the VRAM temperatures of supported cards through hwmon, unlocked or not (default: on)");

int bar1_max_exp = -1;
module_param(bar1_max_exp, int, 0444);
MODULE_PARM_DESC(bar1_max_exp, "Cap the advertised BAR1 size ladder at 2^N MB (default: -1, no cap)");

/* A target card. Those unlocked at boot stay listed, with a reference, to be
 * unlocked again after each resume. */
struct card {
	struct list_head node;
	struct gu_device device;
	bool unlocked;
	bool prepared;
	struct device *pm;
	struct gu_runtime_pm *runtime_pm;
	struct gu_hbm_worker *hbm;
	/* Activation after the stock driver's devinit, see relink_work(). */
	struct delayed_work relink;
	bool relink_bound;
	unsigned int relink_waits;
};

static LIST_HEAD(cards);
static ASYNC_DOMAIN_EXCLUSIVE(unlock_domain);
static struct gpu_unlock_shared shared, runtime_shared;
static bool pm_registered, pm_prepared, pm_suspending;

static void restart_relink(void);

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
	int ret;

	pci_dev_lock(pdev);
	ret = pdev->driver ? -EBUSY : gpu_unlock_run(&card->device, &shared, GU_UNLOCK_BOOT);
	if (!pdev->driver)
		gu_fuses_capture(&card->device, ret);
	pci_dev_unlock(pdev);
	if (ret)
		pci_err(pdev, "gpu_unlock: unlock failed (%d)\n", ret);
	else
		pci_info(pdev, "gpu_unlock: unlocked\n");
	card->unlocked = !ret;
}

/* The parent PCI device has restored config space. The PM core keeps device
 * IRQs disabled and userspace frozen until all of these callbacks finish. */
static int unlock_again(void *data)
{
	struct card *card = data;
	struct pci_dev *pdev = card->device.pdev;
	bool nvidia;
	int ret = 0;

	if (!pm_prepared)
		return 0;
	pci_dev_lock(pdev);
	nvidia = pdev->driver && !strcmp(pdev->driver->name, "nvidia");
	if (nvidia) {
		pci_info(pdev, "gpu_unlock: resume_noirq begin\n");
		ret = gpu_unlock_run(&card->device, &shared, GU_UNLOCK_RESUME);
	}
	pci_dev_unlock(pdev);
	if (!nvidia)
		pci_info(pdev, "gpu_unlock: not bound to nvidia, left as is after resume\n");
	else if (ret)
		pci_err(pdev, "gpu_unlock: unlock after resume failed (%d)\n", ret);
	else
		pci_info(pdev, "gpu_unlock: unlocked again after resume\n");
	return ret;
}

/* PCI access is restored but NVIDIA's runtime callback has not run. The PM
 * core serializes the transition; its caller may hold the PCI device lock. */
static int unlock_runtime(void *data)
{
	struct card *card = data;
	struct pci_dev *pdev = card->device.pdev;
	int ret;

	pci_info(pdev, "gpu_unlock: runtime resume begin\n");
	ret = gpu_unlock_run(&card->device, &runtime_shared, GU_UNLOCK_RUNTIME_RESUME);
	if (ret)
		pci_err(pdev, "gpu_unlock: unlock before runtime resume failed (%d)\n", ret);
	else
		pci_info(pdev, "gpu_unlock: unlock ready for NVIDIA runtime resume\n");
	return ret;
}

/* Serial preparation publishes an immutable cache for the following workers. */
static int prepare_cards(void)
{
	struct card *card;
	int ret, error = 0;

	list_for_each_entry(card, &cards, node) {
		ret = gpu_unlock_load_shared(&card->device, &shared);
		card->prepared = !ret;
		if (ret) {
			pci_err(card->device.pdev, "gpu_unlock: firmware load failed (%d)\n", ret);
			if (!error)
				error = ret;
		}
	}
	return error;
}

/* At boot the NVIDIA module cannot load until every card has finished. */
static void unlock_cards(void)
{
	struct card *card;

	prepare_cards();
	/* Publish the completed cache only after every selected family is prepared. */
	list_for_each_entry(card, &cards, node) {
		if (card->prepared)
			async_schedule_domain(unlock_one, card, &unlock_domain);
	}
	async_synchronize_full_domain(&unlock_domain);
	if (runtime_resume) {
		/* Transfer the boot cache to an immutable runtime cache. It remains
		 * available even when runtime resume happens during system sleep. */
		runtime_shared = shared;
		memset(&shared, 0, sizeof(shared));
	} else {
		gpu_unlock_release_shared(&shared);
	}
}

static void drop_card(struct card *card)
{
	gu_hbm_remove(card->hbm);
	gu_runtime_pm_remove(card->runtime_pm);
	gu_pm_remove(card->pm);
	cancel_delayed_work_sync(&card->relink);
	list_del(&card->node);
	pci_dev_put(card->device.pdev);
	kfree(card);
}

/* Notifiers only manage resources. Hardware work belongs to device PM, before
 * Linux thaws tasks and invokes PM_POST_SUSPEND. No NVIDIA priority is assumed. */
static int gpu_unlock_pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	struct card *card;
	int ret;

	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		WRITE_ONCE(pm_suspending, true);
		list_for_each_entry(card, &cards, node)
			cancel_delayed_work_sync(&card->relink);
		if (event != PM_SUSPEND_PREPARE || !READ_ONCE(resume))
			break;
		ret = prepare_cards();
		if (ret) {
			gpu_unlock_release_shared(&shared);
			WRITE_ONCE(pm_suspending, false);
			restart_relink();
			return notifier_from_errno(ret);
		}
		pm_prepared = true;
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		pm_prepared = false;
		gpu_unlock_release_shared(&shared);
		WRITE_ONCE(pm_suspending, false);
		restart_relink();
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block pm_nb = {
	.notifier_call = gpu_unlock_pm_notify,
};

/* The stock driver resets the link registers when it boots the card. */
static int relink_one(struct card *card)
{
	struct pci_dev *pdev = card->device.pdev;
	const struct gu_family *family = card->device.recipe->family;
	void __iomem *bar;
	u16 command = 0;
	u32 wpr_hi;
	int ret = -ENODEV, pm = 0;

	pci_dev_lock(pdev);
	if (READ_ONCE(pm_suspending)) {
		ret = -EBUSY;
		goto out;
	}
#ifdef GU_PM_ACTIVE_HAS_IGNORE
	pm = pm_runtime_get_if_active(&pdev->dev, true);
#else
	pm = pm_runtime_get_if_active(&pdev->dev);
#endif
	if (!pm) {
		ret = -EAGAIN;
		goto out;
	}
	pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (pdev->driver && !strcmp(pdev->driver->name, "nvidia") &&
	    pdev->current_state == PCI_D0 && (command & PCI_COMMAND_MEMORY) &&
	    (bar = pci_iomap(pdev, 0, 0))) {
		/* Binding persists during suspend. Wait for the driver to publish
		 * its protected firmware region again before touching link policy. */
		wpr_hi = readl(bar + GU_WPR2_ADDR_HI_OFFSET);
		if (!(wpr_hi >> 4) || wpr_hi == ~0u ||
		    (wpr_hi & 0xffff0000u) == 0xbadf0000u)
			ret = -EAGAIN;
		else
			ret = gu_pcie_relink(pdev, bar, pci_resource_len(pdev, 0), family);
		if (!ret && card->device.recipe->chip == 0x170) {
			gu_inforom_log_ga100(&pdev->dev, bar, pci_resource_len(pdev, 0), "relink");
			gu_ecc_log(&pdev->dev, bar, pci_resource_len(pdev, 0), "relink");
		}
		pci_iounmap(pdev, bar);
	}
out:
	if (pm > 0)
		pm_runtime_put(&pdev->dev);
	pci_dev_unlock(pdev);
	return ret;
}

/* Write "all" or a PCI address once nvidia has initialised the cards. Cards
 * whose family has no link speed are not listed. */
static ssize_t relink_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	bool all = sysfs_streq(buf, "all"), found = false;
	struct card *card;
	int ret, error = 0;

	list_for_each_entry(card, &cards, node) {
		if (!card->device.recipe->family->link_speed ||
		    (!all && !sysfs_streq(buf, pci_name(card->device.pdev))))
			continue;
		found = true;
		ret = relink_one(card);
		if (ret && !error)
			error = ret;
	}
	return !found ? -ENODEV : error ? error : count;
}

static struct kobj_attribute relink_attr = __ATTR_WO(relink);
static bool relink_published;

/* Only when an unlocked card can be retrained. */
static void publish_relink(void)
{
	struct card *card;

	list_for_each_entry(card, &cards, node) {
		if (!card->device.recipe->family->link_speed)
			continue;
		if (sysfs_create_file(&THIS_MODULE->mkobj.kobj, &relink_attr.attr))
			pr_err("gpu_unlock: relink not published\n");
		else
			relink_published = true;
		return;
	}
}

/* The stock driver's devinit reprograms the XVE strap shadows, so the Gen2
 * policy is re-applied and the link retrained after it has bound. Bounded:
 * one-second polls for the binding, five seconds for devinit to finish, then
 * three activation attempts five seconds apart. */
#define GU_RELINK_BIND_WAIT	msecs_to_jiffies(1000)
#define GU_RELINK_BIND_WAITS	60
#define GU_RELINK_SETTLE	msecs_to_jiffies(5000)
#define GU_RELINK_ATTEMPTS	3

static void queue_relink(struct card *card, unsigned long delay)
{
	if (!READ_ONCE(pm_suspending))
		schedule_delayed_work(&card->relink, delay);
}

static void restart_relink(void)
{
	struct card *card;

	list_for_each_entry(card, &cards, node) {
		card->relink_bound = false;
		card->relink_waits = 0;
		if (card->device.recipe->family->link_speed)
			queue_relink(card, GU_RELINK_BIND_WAIT);
	}
}

static void relink_work(struct work_struct *work)
{
	struct card *card = container_of(work, struct card, relink.work);
	struct pci_dev *pdev = card->device.pdev;
	bool nvidia;

	if (READ_ONCE(pm_suspending))
		return;
	pci_dev_lock(pdev);
	nvidia = pdev->driver && !strcmp(pdev->driver->name, "nvidia");
	pci_dev_unlock(pdev);
	if (!nvidia) {
		if (++card->relink_waits < GU_RELINK_BIND_WAITS)
			queue_relink(card, GU_RELINK_BIND_WAIT);
		return;
	}
	if (!card->relink_bound) {
		card->relink_bound = true;
		card->relink_waits = 0;
		queue_relink(card, GU_RELINK_SETTLE);
		return;
	}
	if (card->relink_waits++ >= GU_RELINK_ATTEMPTS)
		return;
	if (!relink_one(card))
		return;
	pci_warn(pdev, "gpu_unlock: link not retrained after the driver's pass\n");
	if (card->relink_waits < GU_RELINK_ATTEMPTS)
		queue_relink(card, GU_RELINK_SETTLE);
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

static void report_unmatched(void)
{
	const char *entry, *end;
	struct card *card;
	bool matched;

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
}

static int unlock_targets(void)
{
	struct pci_dev *pdev = NULL;
	struct card *card, *next;
	const struct gu_card *recipe;
	bool all, enumerated = true;
	int ret;

	all = !strcmp(targets, "all");
	while ((pdev = next_supported(pdev, &recipe))) {
		if (!all && !listed(pci_name(pdev)))
			continue;
		card = kzalloc(sizeof(*card), GFP_KERNEL);
		if (!card) {
			pci_err(pdev, "gpu_unlock: card allocation failed\n");
			enumerated = false;
			pci_dev_put(pdev);
			break;
		}
		card->device.pdev = pci_dev_get(pdev);
		card->device.recipe = recipe;
		INIT_DELAYED_WORK(&card->relink, relink_work);
		list_add_tail(&card->node, &cards);
	}
	if (!all && enumerated)
		report_unmatched();
	/* nvidia loads once this returns: every card must be finished by then. */
	unlock_cards();
	ret = gu_overrides_publish();
	if (ret)
		return ret;
	gu_fuses_publish();
	list_for_each_entry_safe(card, next, &cards, node)
		if (!card->unlocked)
			drop_card(card);
	list_for_each_entry(card, &cards, node) {
		if (card->device.recipe->family->vram == GU_VRAM_HBM) {
			card->hbm = gu_hbm_start(card->device.pdev);
			if (IS_ERR(card->hbm)) {
				ret = PTR_ERR(card->hbm);
				card->hbm = NULL;
				pci_err(card->device.pdev, "gpu_unlock: HBM governor registration failed (%d)\n", ret);
				return ret;
			}
		}
		card->pm = gu_pm_add(&card->device.pdev->dev, unlock_again, card);
		if (IS_ERR(card->pm)) {
			ret = PTR_ERR(card->pm);
			card->pm = NULL;
			pci_err(card->device.pdev, "gpu_unlock: PM helper registration failed (%d)\n", ret);
			return ret;
		}
		if (runtime_resume) {
			card->runtime_pm = gu_runtime_pm_add(card->device.pdev,
							   unlock_runtime, card);
			if (IS_ERR(card->runtime_pm)) {
				ret = PTR_ERR(card->runtime_pm);
				card->runtime_pm = NULL;
				pci_warn(card->device.pdev,
					 "gpu_unlock: runtime resume hook unavailable (%d)\n", ret);
			} else {
				pci_info(card->device.pdev, "gpu_unlock: experimental runtime resume hook enabled\n");
			}
		}
	}
	if (!list_empty(&cards)) {
		ret = register_pm_notifier(&pm_nb);
		if (ret)
			return ret;
		pm_registered = true;
	}
	restart_relink();
	publish_relink();
	return 0;
}

static void gpu_unlock_cleanup(void)
{
	struct card *card, *next;

	WRITE_ONCE(pm_suspending, true);
	gu_vram_remove();
	gu_overrides_remove();
	if (relink_published)
		sysfs_remove_file(&THIS_MODULE->mkobj.kobj, &relink_attr.attr);
	if (pm_registered)
		unregister_pm_notifier(&pm_nb);
	list_for_each_entry_safe(card, next, &cards, node)
		drop_card(card);
	gpu_unlock_release_shared(&runtime_shared);
	gu_fuses_remove();
}

static unsigned int gu_sleep_lock(void)
{
#ifdef GU_SLEEP_LOCK_FLAGS
	return lock_system_sleep();
#else
	lock_system_sleep();
	return 0;
#endif
}

static void gu_sleep_unlock(unsigned int flags)
{
#ifdef GU_SLEEP_LOCK_FLAGS
	unlock_system_sleep(flags);
#else
	(void)flags;
	unlock_system_sleep();
#endif
}

static int __init gpu_unlock_init(void)
{
	unsigned int flags = gu_sleep_lock();
	int ret = 0;

	/* Child PM devices cannot be added in the middle of a sleep transition. */
	if (targets && *targets)
		ret = unlock_targets();
	if (!ret && vram_temps)
		gu_vram_publish();
	if (ret)
		gpu_unlock_cleanup();
	gu_sleep_unlock(flags);
	return ret;
}

static void __exit gpu_unlock_exit(void)
{
	unsigned int flags = gu_sleep_lock();

	gpu_unlock_cleanup();
	gu_sleep_unlock(flags);
}

module_init(gpu_unlock_init);
module_exit(gpu_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ampere pre-driver hardware unlock");
MODULE_VERSION("3.0.0");
