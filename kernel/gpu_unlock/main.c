// SPDX-License-Identifier: GPL-2.0-only
#include <linux/async.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#include "unlock_610.h"
#include "recipes.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define GU_MATTR_CONST const
#else
#define GU_MATTR_CONST
#endif

static char *targets;
module_param(targets, charp, 0444);
MODULE_PARM_DESC(targets, "\"all\" or comma-separated PCI addresses of supported Ampere cards to unlock");

static bool resume = true;
module_param(resume, bool, 0644);
MODULE_PARM_DESC(resume, "Unlock the cards again after resume from suspend, which powers them off (default: on)");

/* A target card. Those unlocked at boot stay listed, with a reference, to be
 * unlocked again after each resume. */
struct card {
	struct list_head node;
	struct gu_device device;
	bool unlocked;
	bool prepared;
	int last_error;
};

static LIST_HEAD(cards);
static DEFINE_MUTEX(cards_lock);
static ASYNC_DOMAIN_EXCLUSIVE(unlock_domain);
static struct gpu_unlock_610_shared shared;
static bool pm_registered;
static bool status_file_created;

static const char *error_reason(int err)
{
	switch (err) {
	case 0:
		return "ok";
	case -EBUSY:
		return "driver already bound";
	case -ENODEV:
		return "device not supported or chip mismatch";
	case -ENOENT:
		return "firmware missing";
	case -EINVAL:
		return "invalid parameters or payload";
	case -ENOMEM:
		return "out of memory";
	case -EIO:
		return "I/O error";
	case -ETIMEDOUT:
		return "timeout";
	case -EALREADY:
		return "already unlocked";
	case -ERANGE:
		return "register out of range";
	default:
		return "error";
	}
}

static ssize_t format_status(char *buf, size_t size)
{
	struct card *card;
	ssize_t len = 0;
	unsigned int success = 0, failed = 0, total = 0;

	mutex_lock(&cards_lock);

	if (!targets || !*targets) {
		len = scnprintf(buf, size,
				"status: disabled\n"
				"targets: <none>\n"
				"reason: targets parameter not set\n");
		goto out;
	}

	list_for_each_entry(card, &cards, node) {
		total++;
		if (card->unlocked)
			success++;
		else
			failed++;
	}

	if (total == 0) {
		len = scnprintf(buf, size,
				"status: idle\n"
				"targets: %s\n"
				"reason: no matching supported cards found\n",
				targets);
		goto out;
	}

	if (failed == 0)
		len += scnprintf(buf + len, size - len, "status: patched\n");
	else if (success > 0)
		len += scnprintf(buf + len, size - len, "status: partial\n");
	else
		len += scnprintf(buf + len, size - len, "status: failed\n");

	len += scnprintf(buf + len, size - len,
			 "targets: %s\n"
			 "cards: %u/%u unlocked\n",
			 targets, success, total);

	list_for_each_entry(card, &cards, node) {
		struct pci_dev *pdev = card->device.pdev;
		const char *family = (card->device.recipe && card->device.recipe->family) ?
				     card->device.recipe->family->name : "unknown";
		const char *driver_name;

		pci_dev_lock(pdev);
		driver_name = pdev->driver ? pdev->driver->name : "none";
		pci_dev_unlock(pdev);

		if (card->unlocked) {
			len += scnprintf(buf + len, size - len,
					 "%s: patched (%s, %04x:%04x, driver: %s)\n",
					 pci_name(pdev), family,
					 pdev->vendor, pdev->device, driver_name);
		} else {
			len += scnprintf(buf + len, size - len,
					 "%s: failed (%d: %s, %s, %04x:%04x, driver: %s)\n",
					 pci_name(pdev), card->last_error,
					 error_reason(card->last_error),
					 family, pdev->vendor, pdev->device, driver_name);
		}
	}

out:
	mutex_unlock(&cards_lock);
	return len;
}

static ssize_t status_show(GU_MATTR_CONST struct module_attribute *mattr,
			   struct module_kobject *mk, char *buf)
{
	return format_status(buf, PAGE_SIZE);
}

static struct module_attribute status_attr = {
	.attr = {
		.name = "status",
		.mode = 0444,
	},
	.show = status_show,
};

static int status_param_get(char *buffer, const struct kernel_param *kp)
{
	return (int)format_status(buffer, PAGE_SIZE);
}

static const struct kernel_param_ops status_param_ops = {
	.get = status_param_get,
};
module_param_cb(status, &status_param_ops, NULL, 0444);
MODULE_PARM_DESC(status, "Current module patching and card status");

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
	const char *family = (card->device.recipe && card->device.recipe->family) ?
			     card->device.recipe->family->name : "supported";
	int ret;

	pci_dev_lock(pdev);
	ret = pdev->driver ? -EBUSY : gpu_unlock_610_unlock(&card->device, &shared, false);
	pci_dev_unlock(pdev);
	card->last_error = ret;
	card->unlocked = !ret;
	if (ret == -EBUSY)
		pci_err(pdev, "gpu_unlock: patching failed: driver \"%s\" already bound (-EBUSY)\n",
			pdev->driver ? pdev->driver->name : "unknown");
	else if (ret)
		pci_err(pdev, "gpu_unlock: patching failed (%d: %s)\n",
			ret, error_reason(ret));
	else
		pci_info(pdev, "gpu_unlock: patching successful (%s), card unlocked\n",
			 family);
}

/* After resume, before nvidia resumes the card it suspended. Losing power in
 * suspend to RAM reset the selectors. Runs concurrently for each card. */
static void unlock_again(void *data, async_cookie_t cookie)
{
	struct card *card = data;
	struct pci_dev *pdev = card->device.pdev;
	bool nvidia;
	int ret = 0;

	pci_dev_lock(pdev);
	nvidia = pdev->driver && !strcmp(pdev->driver->name, "nvidia");
	if (nvidia)
		ret = gpu_unlock_610_unlock(&card->device, &shared, true);
	pci_dev_unlock(pdev);
	if (!nvidia) {
		pci_info(pdev, "gpu_unlock: not bound to nvidia, skipped patching after resume\n");
	} else if (ret == -EALREADY) {
		pci_info(pdev, "gpu_unlock: still unlocked after resume (patch intact)\n");
	} else if (ret) {
		card->last_error = ret;
		card->unlocked = false;
		pci_err(pdev, "gpu_unlock: patching after resume failed (%d: %s)\n",
			ret, error_reason(ret));
	} else {
		card->last_error = 0;
		card->unlocked = true;
		pci_info(pdev, "gpu_unlock: patching successful, unlocked again after resume\n");
	}
}

/* Prepare all selected firmware families, then run the ready cards concurrently. */
static void unlock_cards(async_func_t unlock)
{
	struct card *card;
	int ret;

	list_for_each_entry(card, &cards, node) {
		if (unlock == unlock_again && !card->unlocked)
			continue;
		ret = gpu_unlock_610_load_shared(&card->device, &shared);
		card->prepared = !ret;
		if (ret) {
			card->last_error = ret;
			pci_err(card->device.pdev, "gpu_unlock: firmware load failed (%d: %s)\n",
				ret, error_reason(ret));
		}
	}
	/* Publish the completed cache only after every selected family is prepared. */
	list_for_each_entry(card, &cards, node) {
		if (card->prepared && (unlock != unlock_again || card->unlocked))
			async_schedule_domain(unlock, card, &unlock_domain);
	}
	async_synchronize_full_domain(&unlock_domain);
	gpu_unlock_610_release_shared(&shared);
}

static void drop_card(struct card *card)
{
	list_del(&card->node);
	pci_dev_put(card->device.pdev);
	kfree(card);
}

/* nvidia resumes its cards from its own notifier, at the default priority 0,
 * once this returns: every card must be finished by then. */
static int gpu_unlock_pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	if (event == PM_POST_SUSPEND && resume) {
		mutex_lock(&cards_lock);
		unlock_cards(unlock_again);
		mutex_unlock(&cards_lock);
	}
	return NOTIFY_DONE;
}

static struct notifier_block pm_nb = {
	.notifier_call = gpu_unlock_pm_notify,
	.priority = 1,
};

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
			pr_err("gpu_unlock: no supported card found for patching at \"%.*s\"\n",
			       (int)(end - entry), entry);
	}
}

static int __init gpu_unlock_init(void)
{
	struct pci_dev *pdev = NULL;
	struct card *card;
	const struct gu_card *recipe;
	bool all, enumerated = true;
	unsigned int success = 0, failed = 0;

	if (!sysfs_create_file(&THIS_MODULE->mkobj.kobj, &status_attr.attr))
		status_file_created = true;

	/* Loaded ahead of nvidia on every boot; the command line is the opt-in. */
	if (!targets || !*targets) {
		pr_info("gpu_unlock: patching disabled (targets parameter not set)\n");
		return 0;
	}
	all = !strcmp(targets, "all");
	mutex_lock(&cards_lock);
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
		list_add_tail(&card->node, &cards);
	}
	if (!all && enumerated)
		report_unmatched();

	if (list_empty(&cards)) {
		pr_warn("gpu_unlock: no supported cards found for patching (targets=\"%s\")\n",
			targets);
		mutex_unlock(&cards_lock);
		return 0;
	}

	/* nvidia loads once this returns: every card must be finished by then. */
	unlock_cards(unlock_one);

	list_for_each_entry(card, &cards, node) {
		if (card->unlocked)
			success++;
		else
			failed++;
	}

	if (failed == 0)
		pr_info("gpu_unlock: patching completed: %u card(s) successfully patched\n",
			success);
	else if (success > 0)
		pr_warn("gpu_unlock: patching completed with errors: %u card(s) patched, %u failed\n",
			success, failed);
	else
		pr_err("gpu_unlock: patching unsuccessful: all %u card(s) failed\n",
			failed);

	if (success > 0) {
		register_pm_notifier(&pm_nb);
		pm_registered = true;
	}
	mutex_unlock(&cards_lock);
	return 0;
}

static void __exit gpu_unlock_exit(void)
{
	struct card *card, *next;

	if (status_file_created)
		sysfs_remove_file(&THIS_MODULE->mkobj.kobj, &status_attr.attr);
	if (pm_registered)
		unregister_pm_notifier(&pm_nb);
	mutex_lock(&cards_lock);
	list_for_each_entry_safe(card, next, &cards, node)
		drop_card(card);
	mutex_unlock(&cards_lock);
}

module_init(gpu_unlock_init);
module_exit(gpu_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ampere pre-driver hardware unlock");
MODULE_VERSION("2.0.0");
