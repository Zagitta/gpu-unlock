// SPDX-License-Identifier: GPL-2.0-only
#include <linux/async.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>

#include "unlock_610.h"
#include "recipes.h"

static char *targets;
module_param(targets, charp, 0444);
MODULE_PARM_DESC(targets, "\"all\" or comma-separated PCI addresses of supported Ampere cards to unlock");

static bool direct = true;
module_param(direct, bool, 0444);
MODULE_PARM_DESC(direct, "GA10x: write the speed selectors from the signed Booter itself, never opening the privilege masks (default: on)");

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
};

static LIST_HEAD(cards);
static ASYNC_DOMAIN_EXCLUSIVE(unlock_domain);
static struct gpu_unlock_610_shared shared;

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
	ret = pdev->driver ? -EBUSY : gpu_unlock_610_unlock(&card->device, &shared, false, direct);
	pci_dev_unlock(pdev);
	if (ret)
		pci_err(pdev, "gpu_unlock: unlock failed (%d)\n", ret);
	else
		pci_info(pdev, "gpu_unlock: unlocked\n");
	card->unlocked = !ret;
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
		ret = gpu_unlock_610_unlock(&card->device, &shared, true, direct);
	pci_dev_unlock(pdev);
	if (!nvidia)
		pci_info(pdev, "gpu_unlock: not bound to nvidia, left as is after resume\n");
	else if (ret == -EALREADY)
		pci_info(pdev, "gpu_unlock: still unlocked after resume\n");
	else if (ret)
		pci_err(pdev, "gpu_unlock: unlock after resume failed (%d)\n", ret);
	else
		pci_info(pdev, "gpu_unlock: unlocked again after resume\n");
}

/* Prepare all selected firmware families, then run the ready cards concurrently. */
static void unlock_cards(async_func_t unlock)
{
	struct card *card;
	int ret;

	list_for_each_entry(card, &cards, node) {
		ret = gpu_unlock_610_load_shared(&card->device, &shared);
		card->prepared = !ret;
		if (ret)
			pci_err(card->device.pdev, "gpu_unlock: firmware load failed (%d)\n", ret);
	}
	/* Publish the completed cache only after every selected family is prepared. */
	list_for_each_entry(card, &cards, node) {
		if (card->prepared)
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
	if (event == PM_POST_SUSPEND && resume)
		unlock_cards(unlock_again);
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
			pr_err("gpu_unlock: no supported card at \"%.*s\"\n",
			       (int)(end - entry), entry);
	}
}

static int __init gpu_unlock_init(void)
{
	struct pci_dev *pdev = NULL;
	struct card *card, *next;
	const struct gu_card *recipe;
	bool all, enumerated = true;

	/* Loaded ahead of nvidia on every boot; the command line is the opt-in. */
	if (!targets || !*targets)
		return 0;
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
		list_add_tail(&card->node, &cards);
	}
	if (!all && enumerated)
		report_unmatched();
	/* nvidia loads once this returns: every card must be finished by then. */
	unlock_cards(unlock_one);
	list_for_each_entry_safe(card, next, &cards, node)
		if (!card->unlocked)
			drop_card(card);
	if (!list_empty(&cards))
		register_pm_notifier(&pm_nb);
	return 0;
}

static void __exit gpu_unlock_exit(void)
{
	struct card *card, *next;

	if (!list_empty(&cards))
		unregister_pm_notifier(&pm_nb);
	list_for_each_entry_safe(card, next, &cards, node)
		drop_card(card);
}

module_init(gpu_unlock_init);
module_exit(gpu_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ampere pre-driver hardware unlock");
MODULE_VERSION("2.0.0");
