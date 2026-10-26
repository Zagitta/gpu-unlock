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

static char *targets;
module_param(targets, charp, 0444);
MODULE_PARM_DESC(targets, "\"all\" or comma-separated PCI addresses of GA102/GA103/GA104/GA106 cards to unlock");

static bool resume = true;
module_param(resume, bool, 0644);
MODULE_PARM_DESC(resume, "Unlock the cards again after resume from suspend, which powers them off (default: on)");

/* GA102, GA103, GA104, GA106 and GA107 GPU functions in pci.ids, excluding
 * audio functions and the professional (GL) parts, which already run BF16 at
 * full rate. GA103 and GA106 also exclude other mobile parts.
 */
static const unsigned short supported_ids[] = {
	/* GA102 */
	0x2200, 0x2203, 0x2204, 0x2205, 0x2206, 0x2207, 0x2208, 0x220a,
	0x220d, 0x2216, 0x222b, 0x222f,
	/* GA104 */
	0x2480, 0x2482, 0x2483, 0x2484, 0x2486, 0x2487, 0x2488, 0x2489,
	0x248a, 0x248c, 0x248d, 0x248e, 0x249c, 0x249d, 0x249f, 0x24a0,
	0x24a4, 0x24ac, 0x24ad, 0x24af, 0x24bf, 0x24c0, 0x24c7, 0x24c8,
	0x24c9, 0x24dc, 0x24dd, 0x24df, 0x24e0,
	/* GA103 (untested) */
	0x2414,
	/* GA106 (validated only on RTX 3060 12GB 0x2504) */
	0x2501, 0x2503, 0x2504, 0x2505, 0x2507, 0x2508, 0x2509, 0x252f,
	0x2544,
	/* GA107 (validated only on a Lenovo RTX 3050 6GB Laptop, subsystem
	 * 17aa:3c90; see docs/Extending to a new laptop GPU.md). Cold-probe
	 * unlock did not survive nvidia binding on that board; the resume
	 * re-unlock above did. */
	0x25ec,
};

/* A target card. Those unlocked at boot stay listed, with a reference, to be
 * unlocked again after each resume. */
struct card {
	struct list_head node;
	struct pci_dev *pdev;
	bool unlocked;
};

static LIST_HEAD(cards);
static ASYNC_DOMAIN_EXCLUSIVE(unlock_domain);
static struct gpu_unlock_610_shared shared;

/* The next supported NVIDIA card after from, like pci_get_device(). */
static struct pci_dev *next_supported(struct pci_dev *from)
{
	struct pci_dev *pdev = from;
	unsigned int i;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev)))
		for (i = 0; i < ARRAY_SIZE(supported_ids); i++)
			if (pdev->device == supported_ids[i])
				return pdev;
	return NULL;
}

/* At boot, before nvidia binds the card. Runs concurrently for each card. */
static void unlock_one(void *data, async_cookie_t cookie)
{
	struct card *card = data;
	struct pci_dev *pdev = card->pdev;
	int ret;

	pci_dev_lock(pdev);
	ret = pdev->driver ? -EBUSY : gpu_unlock_610_unlock(pdev, &shared, false);
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
	struct pci_dev *pdev = card->pdev;
	bool nvidia;
	int ret = 0;

	pci_dev_lock(pdev);
	nvidia = pdev->driver && !strcmp(pdev->driver->name, "nvidia");
	if (nvidia)
		ret = gpu_unlock_610_unlock(pdev, &shared, true);
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

/* Unlock every listed card concurrently, with the GSP firmware loaded once. */
static void unlock_cards(async_func_t unlock)
{
	struct card *card;
	int ret;

	list_for_each_entry(card, &cards, node) {
		if (!shared.gsp) {
			ret = gpu_unlock_610_load_shared(card->pdev, &shared);
			if (ret) {
				pci_err(card->pdev, "gpu_unlock: firmware load failed (%d)\n", ret);
				break;
			}
		}
		async_schedule_domain(unlock, card, &unlock_domain);
	}
	async_synchronize_full_domain(&unlock_domain);
	gpu_unlock_610_release_shared(&shared);
}

static void drop_card(struct card *card)
{
	list_del(&card->node);
	pci_dev_put(card->pdev);
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
	struct pci_dev *pdev;

	for (entry = targets; *entry; entry = *end ? end + 1 : end) {
		end = strchrnul(entry, ',');
		pdev = NULL;
		while ((pdev = next_supported(pdev)))
			if (strlen(pci_name(pdev)) == end - entry &&
			    !strncasecmp(pci_name(pdev), entry, end - entry))
				break;
		if (pdev)
			pci_dev_put(pdev);
		else
			pr_err("gpu_unlock: no supported card at \"%.*s\"\n",
			       (int)(end - entry), entry);
	}
}

static int __init gpu_unlock_init(void)
{
	struct pci_dev *pdev = NULL;
	struct card *card, *next;
	bool all;

	/* Loaded ahead of nvidia on every boot; the command line is the opt-in. */
	if (!targets || !*targets)
		return 0;
	all = !strcmp(targets, "all");
	if (!all)
		report_unmatched();
	while ((pdev = next_supported(pdev))) {
		if (!all && !listed(pci_name(pdev)))
			continue;
		card = kzalloc(sizeof(*card), GFP_KERNEL);
		if (!card) {
			pci_dev_put(pdev);
			break;
		}
		card->pdev = pci_dev_get(pdev);
		list_add_tail(&card->node, &cards);
	}
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
MODULE_DESCRIPTION("Ampere pre-driver BF16 unlock");
MODULE_VERSION("1.0.0");
