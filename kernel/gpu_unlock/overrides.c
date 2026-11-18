// SPDX-License-Identifier: GPL-2.0-only
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kmod.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#include "firmware.h"
#include "overrides.h"
#include "recipes.h"
#include "registers.h"
#include "vbios.h"

struct override {
	struct list_head node;
	struct pci_dev *pdev;
	struct bin_attribute attr;
	u8 index;
	char name[sizeof("ffffffff-FF")];
	u8 data[];
};

static LIST_HEAD(records);
static DEFINE_MUTEX(records_lock);
static struct kobject *directory, *firmware;
static char *config;
static bool legacy_overrides, notifier_registered;
module_param(legacy_overrides, bool, 0600);
MODULE_PARM_DESC(legacy_overrides, "Deliver VBIOS overrides through legacy NVIDIA procfs (set by prepare-overrides)");

static int override_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	struct device *dev = data;
	struct override *record;
	struct pci_dev *pdev;
	char *argv[] = { "/usr/lib/gpu-unlock/apply-procfs-override", NULL, NULL, NULL };
	char *env[] = { "HOME=/", "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
	int ret;

	if (event != BUS_NOTIFY_BOUND_DRIVER || !READ_ONCE(legacy_overrides) ||
	    !dev->driver || strcmp(dev->driver->name, "nvidia"))
		return NOTIFY_DONE;
	pdev = to_pci_dev(dev);
	list_for_each_entry(record, &records, node) {
		if (record->pdev != pdev)
			continue;
		if (!dev->driver->owner || dev->driver->owner->state != MODULE_STATE_COMING) {
			pci_warn(pdev, "gpu_unlock: legacy VBIOS overrides require initial NVIDIA module loading; reboot after a driver rebind or hotplug\n");
			return NOTIFY_DONE;
		}
		/* PCI probing has finished, but initial module loading has not
		 * published NVIDIA's character devices yet. The helper checks
		 * that adapter initialization is deferred before writing procfs.
		 * Never take the already-held PCI lock or load modules here. */
		argv[1] = (char *)pci_name(pdev);
		argv[2] = record->name;
		ret = call_usermodehelper(argv[0], argv, env, UMH_WAIT_PROC);
		/* UMH_WAIT_PROC returns the child's wait status. */
		if (ret == (10 << 8))
			pci_warn(pdev, "gpu_unlock: legacy VBIOS overrides require options nvidia NVreg_GpuInitOnProbe=0; rebuild the initramfs if NVIDIA loads there, then reboot\n");
		else if (ret)
			pci_warn(pdev, "gpu_unlock: legacy VBIOS override helper failed (%d); check gpu_unlock messages\n", ret);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static struct notifier_block override_nb = {
	.notifier_call = override_notify,
};

static ssize_t read_binary(struct file *file, struct kobject *kobj,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
			   const struct bin_attribute *attr,
#else
			   struct bin_attribute *attr,
#endif
			   char *buf, loff_t off, size_t size)
{
	return memory_read_from_buffer(buf, size, &off, attr->private, attr->size);
}

static void init_binary(struct bin_attribute *attr, const char *name,
			void *data, size_t size)
{
	/* A zero-size sysfs binary file is unbounded to kernfs. Keep the
	 * buffer's own bounds even when the generated configuration is empty. */
	*attr = (struct bin_attribute)__BIN_ATTR(override, 0444, read_binary, NULL, 0);
	sysfs_bin_attr_init(attr);
	attr->attr.name = name;
	attr->private = data;
	attr->size = size;
}

static struct bin_attribute config_attr;

static void free_overrides(struct list_head *head)
{
	struct override *record, *next;

	list_for_each_entry_safe(record, next, head, node) {
		list_del(&record->node);
		pci_dev_put(record->pdev);
		kfree(record);
	}
}

static int add_override(struct list_head *head, struct pci_dev *pdev,
			const u8 *data, size_t size)
{
	struct override *record;
	/* The pending list contains only this GPU's successfully added records. */
	unsigned int index = list_empty(head) ? 0 :
		list_last_entry(head, struct override, node)->index + 1;

	if (index > 0xff)
		return -E2BIG;

	record = kzalloc(sizeof(*record) + size, GFP_KERNEL);
	if (!record)
		return -ENOMEM;
	record->pdev = pci_dev_get(pdev);
	record->index = index;
	memcpy(record->data, data, size);
	init_binary(&record->attr, record->name, record->data, size);
	list_add_tail(&record->node, head);
	return 0;
}

static int capture_ecc(struct list_head *pending, struct pci_dev *pdev,
		       const u8 *rom, size_t size)
{
	u8 data[11];
	int ret = gpu_unlock_vbios_ecc_override(rom, size, data);

	if (ret == -EALREADY) {
		pci_info(pdev, "gpu_unlock: board VBIOS already advertises ECC\n");
		return 0;
	}
	if (ret) {
		pci_err(pdev, "gpu_unlock: cannot generate ECC VBIOS override (%d)\n", ret);
		return ret;
	}
	ret = add_override(pending, pdev, data, sizeof(data));
	if (!ret)
		pci_info(pdev, "gpu_unlock: generated ECC override BIOS=%02x.%02x.%02x.%02x.%02x offset=%#x value=%#x\n",
			 data[0], data[1], data[2], data[3], data[4],
			 data[5] | data[6] << 8 | data[7] << 16 | data[8] << 24, data[10]);
	return ret;
}

static int capture_power(struct list_head *pending, struct pci_dev *pdev,
			 u8 *rom, size_t size, u32 minimum_mw, u32 maximum_mw)
{
	u8 power[22], checksum[11];
	int ret = gpu_unlock_vbios_power_overrides(rom, size, minimum_mw, maximum_mw,
						 power, checksum);

	if (ret) {
		if (ret == -EALREADY)
			pci_info(pdev, "gpu_unlock: board VBIOS already provides the requested power range\n");
		else
			pci_warn(pdev, "gpu_unlock: power-limit override unavailable for this VBIOS (%d)\n", ret);
		return 0;
	}
	ret = add_override(pending, pdev, power, sizeof(power));
	if (!ret)
		ret = add_override(pending, pdev, checksum, sizeof(checksum));
	if (!ret)
		pci_info(pdev, "gpu_unlock: generated power-limit override with factory default preserved\n");
	return ret;
}

static int capture_gpc_offset(struct list_head *pending, struct pci_dev *pdev,
			      u8 *rom, size_t size, u16 offset_mhz)
{
	u8 data[14], checksum[11];
	int ret = gpu_unlock_vbios_gpc_offset_overrides(rom, size, offset_mhz, data, checksum);

	if (ret) {
		if (ret == -EALREADY)
			pci_info(pdev, "gpu_unlock: board VBIOS already provides a core-offset range\n");
		else
			pci_warn(pdev, "gpu_unlock: core-offset override unavailable for this VBIOS (%d)\n", ret);
		return 0;
	}
	ret = add_override(pending, pdev, data, sizeof(data));
	if (!ret)
		ret = add_override(pending, pdev, checksum, sizeof(checksum));
	if (!ret)
		pci_info(pdev, "gpu_unlock: generated +/- %u MHz core-offset range; no offset selected\n", offset_mhz);
	return ret;
}

static int capture_p8(struct list_head *pending, struct pci_dev *pdev,
		      const u8 *rom, size_t size, u16 minimum_mhz)
{
	u8 p8[75], checksum[11];
	int ret = gpu_unlock_vbios_p8_overrides(rom, size, minimum_mhz, p8, checksum);

	if (ret) {
		if (ret == -EALREADY)
			pci_info(pdev, "gpu_unlock: board VBIOS already provides P8\n");
		else
			pci_warn(pdev, "gpu_unlock: P8 override unavailable for this VBIOS (%d)\n", ret);
		return 0;
	}
	ret = add_override(pending, pdev, p8, sizeof(p8));
	if (!ret)
		ret = add_override(pending, pdev, checksum, sizeof(checksum));
	if (!ret)
		pci_info(pdev, "gpu_unlock: generated P8 override with %u MHz idle floors; factory P0 preserved\n",
			 minimum_mhz);
	return ret;
}

int gu_overrides_capture(const struct gu_device *device)
{
	struct pci_dev *pdev = device->pdev;
	const struct gu_card *card = device->recipe;
	const struct gu_rom_recipe *recipe = &device->recipe->family->rom;
	resource_size_t len = pci_resource_len(pdev, 0);
	LIST_HEAD(pending);
	void __iomem *bar;
	size_t size;
	u8 *rom;
	int ret;

	if (!recipe->ecc && !recipe->p8_min_mhz && !recipe->gpc_offset_mhz &&
	    !card->power_min_mw && !card->power_max_mw)
		return 0;
	bar = pci_iomap(pdev, 0, 0);
	if (!bar)
		return -ENOMEM;
	/* Advertise the capability even when EEN currently disables ECC, so
	 * nvidia-smi can request it for the next boot. Hardware selection still
	 * follows EEN in the unlock transaction. */
	if (recipe->min_subrevision) {
		u32 revision;

		if (len < GU_OPT_SUBREVISION_OFFSET + 4) {
			ret = -ERANGE;
			goto unmap;
		}
		revision = readl(bar + GU_OPT_SUBREVISION_OFFSET);
		if (revision == ~0u || (revision & 0xffff0000u) == 0xbadf0000u) {
			ret = -EIO;
			goto unmap;
		}
		if ((revision & 0xf) < recipe->min_subrevision) {
			pci_info(pdev, "gpu_unlock: silicon revision does not support the VBIOS override recipe\n");
			ret = 0;
			goto unmap;
		}
	}
	if (len <= GU_PROM_DATA_OFFSET) {
		ret = -ERANGE;
		goto unmap;
	}
	size = min_t(resource_size_t, 1024 * 1024, len - GU_PROM_DATA_OFFSET);
	rom = kvmalloc(size, GFP_KERNEL);
	if (!rom) {
		ret = -ENOMEM;
		goto free;
	}
	memcpy_fromio(rom, bar + GU_PROM_DATA_OFFSET, size);
	ret = recipe->ecc ? capture_ecc(&pending, pdev, rom, size) : 0;
	if (!ret && (card->power_min_mw || card->power_max_mw))
		ret = capture_power(&pending, pdev, rom, size, card->power_min_mw, card->power_max_mw);
	if (!ret && recipe->gpc_offset_mhz)
		ret = capture_gpc_offset(&pending, pdev, rom, size, recipe->gpc_offset_mhz);
	if (!ret && recipe->p8_min_mhz)
		ret = capture_p8(&pending, pdev, rom, size, recipe->p8_min_mhz);
	if (ret)
		goto free;
	/* Each card publishes its complete bundle, even with parallel unlocks. */
	mutex_lock(&records_lock);
	list_splice_tail_init(&pending, &records);
	mutex_unlock(&records_lock);
free:
	free_overrides(&pending);
	kvfree(rom);
unmap:
	pci_iounmap(pdev, bar);
	return ret;
}

int gu_overrides_publish(void)
{
	struct override *record;
	struct pci_dev *previous = NULL;
	unsigned int bundle = 0;
	size_t size = 1, used = 0;
	int ret;

	list_for_each_entry(record, &records, node)
		size += 160;
	config = kzalloc(size, GFP_KERNEL);
	if (!config)
		return -ENOMEM;
	if (!list_empty(&records)) {
		used += scnprintf(config + used, size - used,
			"options nvidia NVreg_RegistryBinaryFilePerDevice=\"");
		list_for_each_entry(record, &records, node) {
			if (record->pdev != previous) {
				if (previous)
					bundle++;
				used += scnprintf(config + used, size - used, "pci=%s;", pci_name(record->pdev));
				previous = record->pdev;
			}
			/* Capture completes in parallel; assign names to the final bundles. */
			snprintf(record->name, sizeof(record->name), "%x-%02X", bundle, record->index);
			used += scnprintf(config + used, size - used,
				"romOverride%02X=gpu-unlock/%s;", record->index, record->name);
		}
		used += scnprintf(config + used, size - used, "\"\n");
	}
	directory = kobject_create_and_add("gpu_unlock", kernel_kobj);
	if (!directory)
		return -ENOMEM;
	firmware = kobject_create_and_add("overrides", directory);
	if (!firmware)
		return -ENOMEM;
	list_for_each_entry(record, &records, node) {
		ret = sysfs_create_bin_file(firmware, &record->attr);
		if (ret)
			return ret;
	}
	init_binary(&config_attr, "nvidia.conf", config, used);
	ret = sysfs_create_bin_file(directory, &config_attr);
	if (ret || list_empty(&records))
		return ret;
	ret = bus_register_notifier(&pci_bus_type, &override_nb);
	if (!ret)
		notifier_registered = true;
	return ret;
}

void gu_overrides_remove(void)
{
	if (notifier_registered) {
		bus_unregister_notifier(&pci_bus_type, &override_nb);
		notifier_registered = false;
	}
	if (firmware) {
		kobject_del(firmware);
		kobject_put(firmware);
	}
	if (directory) {
		kobject_del(directory);
		kobject_put(directory);
	}
	free_overrides(&records);
	kfree(config);
}
