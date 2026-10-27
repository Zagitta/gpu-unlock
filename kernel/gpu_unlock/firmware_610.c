// SPDX-License-Identifier: GPL-2.0-only
/* Firmware format validation and immutable assets for one coordination round. */
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "diagnostic.h"
#include "firmware_610.h"
#include "recipes.h"

#define BOOTER_MAGIC 0x314c5442u

/* CONFIG_FW_CACHE attaches name records to devres even though this is not
 * devm_request_firmware(). release_firmware() does not remove them, and
 * really_probe() refuses a device with any existing devres. Scope each
 * synchronous request so only its cache records are released; the returned
 * firmware remains owned by the caller.
 */
static int request_scoped(struct device *dev, const struct firmware **fw, const char *name)
{
	void *group;
	int ret, released;

	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group) {
		dev_err(dev, "asset request failed: %s error=%d\n", name, -ENOMEM);
		return -ENOMEM;
	}
	dev_dbg(dev, "asset request: %s\n", name);
	ret = request_firmware(fw, name, dev);
	released = devres_release_group(dev, group);
	if (ret)
		dev_err(dev, "asset request failed: %s error=%d\n", name, ret);
	dev_dbg(dev, "asset result: %s error=%d bytes=%zu\n",
		name, ret, ret ? (size_t)0 : (*fw)->size);
	dev_dbg(dev, "asset cache cleanup: %s devres_released=%d\n", name, released);
	return ret;
}

static int validate_booter(const struct firmware *fw, struct gpu_unlock_booter_hdr *out,
			   const struct gu_booter_expect *expect)
{
	const u8 *data;
	u32 magic, version, image_size, sig_bytes;
	struct gpu_unlock_booter_hdr hdr;

	data = fw->data;
	if (fw->size < 72)
		return GU_DIAG(-EINVAL);
	memcpy(&magic, data, sizeof(magic));
	memcpy(&version, data + 4, sizeof(version));
	if (magic != BOOTER_MAGIC || version != 1)
		return GU_DIAG(-EINVAL);
	memcpy(&hdr, data + 8, sizeof(hdr));
	memcpy(&image_size, data + 8 + sizeof(hdr), sizeof(image_size));
	memcpy(&sig_bytes, data + 12 + sizeof(hdr), sizeof(sig_bytes));
	if (!hdr.ucode_id || hdr.num_apps != 1 || sig_bytes != hdr.sig_bytes ||
	    image_size != expect->image_size ||
	    fw->size != 72u + image_size + sig_bytes)
		return GU_DIAG(-EINVAL);
	/* Keep the signed GA100 and AD102 production geometries distinct. */
	if (hdr.ucode_id != 3 || hdr.engine_id != 1 || hdr.num_sigs != expect->num_sigs ||
	    sig_bytes != expect->sig_bytes || hdr.os_code_off != 0 ||
	    hdr.os_code_size != 256 || hdr.os_data_off != expect->os_data_off ||
	    hdr.os_data_size != expect->os_data_size || hdr.app_code_off != 256 ||
	    hdr.app_code_size != expect->app_code_size || hdr.app_data_off != 256 ||
	    hdr.app_data_size != 0 || hdr.patch_loc != expect->patch_loc)
		return GU_DIAG(-EINVAL);
	*out = hdr;
	return 0;
}

static void release_bundle(struct gpu_unlock_610_firmware *assets)
{
	release_firmware(assets->gsp);
	release_firmware(assets->boot);
	memset(assets, 0, sizeof(*assets));
}

static int prepare_family(struct pci_dev *pdev, struct gpu_unlock_610_firmware *assets,
			  const struct gu_fw_desc *load)
{
	const struct firmware *desc = NULL;
	int ret;

	ret = request_scoped(&pdev->dev, &assets->gsp,
			     load->gsp);
	if (ret)
		return ret;
	ret = gpu_unlock_elf_section(assets->gsp->data, assets->gsp->size,
				    ".fwimage", &assets->image_span);
	if (ret)
		return ret;
	ret = gpu_unlock_elf_section(assets->gsp->data, assets->gsp->size,
				    load->signature_section, &assets->sig_span);
	if (ret)
		return ret;
	if (!assets->image_span.size || assets->image_span.size > 128ull << 20 ||
	    assets->sig_span.size != 4096)
		return -EINVAL;
	ret = gpu_unlock_radix_plan(assets->image_span.size, &assets->radix);
	if (ret)
		return ret;
	ret = request_scoped(&pdev->dev, &assets->boot, load->boot);
	if (ret)
		return ret;
	if (assets->boot->size != load->boot_size)
		return -EINVAL;
	ret = request_scoped(&pdev->dev, &desc, load->desc);
	if (ret)
		return ret;
	ret = gpu_unlock_boot_desc_parse(desc->data, desc->size,
					assets->boot->size, &assets->boot_desc);
	dev_dbg(&pdev->dev, "asset geometry: image=%llu signature=%llu boot=%u code=%#x data=%#x manifest=%#x parse=%d\n",
		(unsigned long long)assets->image_span.size, (unsigned long long)assets->sig_span.size,
		assets->boot_desc.image_size, assets->boot_desc.code_offset,
		assets->boot_desc.data_offset, assets->boot_desc.manifest_offset, ret);
	release_firmware(desc);
	return ret;
}

static struct gpu_unlock_610_firmware *
claim_bundle(struct gpu_unlock_610_shared *shared, const struct gu_fw_desc *fw)
{
	struct gpu_unlock_610_firmware *slot;
	unsigned int i;

	slot = (struct gpu_unlock_610_firmware *)gpu_unlock_610_bundle_find(shared, fw);
	if (slot)
		return slot;
	for (i = 0; i < GU_SHARED_BUNDLES; i++) {
		if (!shared->bundle[i].key) {
			shared->bundle[i].key = fw;
			return &shared->bundle[i];
		}
	}
	return NULL;
}

static int ensure_booter(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared,
			 const struct gu_booter *desc)
{
	struct gpu_unlock_610_booter *slot = NULL;
	const struct firmware *fw = NULL;
	struct gpu_unlock_booter_hdr hdr;
	unsigned int i;
	int ret;

	if (gpu_unlock_610_booter_find(shared, desc))
		return 0;
	for (i = 0; i < GU_SHARED_BOOTERS; i++) {
		if (!shared->booter[i].key) {
			slot = &shared->booter[i];
			break;
		}
	}
	if (!slot)
		return -ENOSPC;
	ret = request_scoped(&pdev->dev, &fw, desc->file);
	if (ret)
		return ret;
	ret = validate_booter(fw, &hdr, &desc->expect);
	if (ret) {
		dev_err(&pdev->dev, "invalid SEC2 Booter bundle: error=%d\n", ret);
		release_firmware(fw);
		return ret;
	}
	slot->key = desc;
	slot->fw = fw;
	slot->hdr = hdr;
	return 0;
}

static int ensure_loader(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared,
			 const char *name)
{
	const struct firmware *loader = NULL;
	int ret;

	if (!name)
		return 0;
	if (shared->loader)
		return shared->loader_name == name ? 0 : -EINVAL;
	ret = request_scoped(&pdev->dev, &loader, name);
	if (ret)
		return ret;
	if (loader->size != 768) {
		dev_err(&pdev->dev, "invalid generic Falcon loader: bytes=%zu\n", loader->size);
		release_firmware(loader);
		return -EINVAL;
	}
	shared->loader = loader;
	shared->loader_name = name;
	return 0;
}

int gpu_unlock_610_load_shared(const struct gu_device *device, struct gpu_unlock_610_shared *shared)
{
	struct pci_dev *pdev;
	const struct gu_family *family;
	const struct gu_fw_desc *load;
	struct gpu_unlock_610_firmware *assets;
	int ret;

	if (!device->recipe || !device->recipe->family || !device->recipe->family->fw ||
	    !device->recipe->family->booter)
		return -ENODEV;
	pdev = device->pdev;
	family = device->recipe->family;
	/* The payload belongs to the Booter image; refuse a pairing the runner
	 * could not use instead of tearing it apart per card later. */
	if (!family->booter->payload || gu_payload_check(family->booter->payload))
		return -EINVAL;
	load = family->fw;
	assets = claim_bundle(shared, load);
	if (!assets)
		return -ENOSPC;
	if (assets->prepared)
		return assets->error;
	ret = prepare_family(pdev, assets, load);
	if (ret)
		goto out;
	ret = ensure_booter(pdev, shared, family->booter);
	if (ret)
		goto out;
	ret = ensure_loader(pdev, shared, family->loader);
out:
	if (ret)
		release_bundle(assets);
	assets->key = load;
	assets->error = ret;
	assets->prepared = true;
	return ret;
}

void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared)
{
	unsigned int i;

	for (i = 0; i < GU_SHARED_BUNDLES; i++)
		release_bundle(&shared->bundle[i]);
	for (i = 0; i < GU_SHARED_BOOTERS; i++)
		release_firmware(shared->booter[i].fw);
	release_firmware(shared->loader);
	memset(shared, 0, sizeof(*shared));
}

MODULE_FIRMWARE(GU_FW_BOOTER_GA100);
MODULE_FIRMWARE(GU_FW_BOOTER_GA102);
MODULE_FIRMWARE(GU_FW_LOADER);
MODULE_FIRMWARE(GU_FW_GSP_GA10X);
MODULE_FIRMWARE(GU_FW_BOOT_GA102);
MODULE_FIRMWARE(GU_FW_DESC_GA102);
MODULE_FIRMWARE(GU_FW_GSP_GA100);
MODULE_FIRMWARE(GU_FW_BOOT_GA100);
MODULE_FIRMWARE(GU_FW_DESC_GA100);
MODULE_FIRMWARE(GU_FW_BOOTER_AD102);
MODULE_FIRMWARE(GU_FW_BOOT_AD102);
MODULE_FIRMWARE(GU_FW_DESC_AD102);
