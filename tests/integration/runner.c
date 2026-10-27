// SPDX-License-Identifier: GPL-2.0-only
/* One exported entry for the integration harness. The driver, shim and
 * device are linked into one shared object; this function is the only
 * boundary Python crosses. */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "device/gd.h"
#include "shim/compat.h"
#include "../../kernel/gpu_unlock/firmware_610.h"
#include "../../kernel/gpu_unlock/recipes.h"
#include "../../kernel/gpu_unlock/unlock_610.h"
#include "../../kernel/gpu_unlock/vbios_610.h"
#include "device/aes.h"
#include "device/wpr2.h"

#if defined(__GNUC__)
#define GD_EXPORT __attribute__((visibility("default")))
#else
#define GD_EXPORT
#endif

struct gd_fwsec_info {
	uint32_t version, image_off, image_size, data_off, imem_size, dmem_size, interface_off;
};

GD_EXPORT int gd_fwsec_extract(const char *rom_path, struct gd_fwsec_info *info)
{
	FILE *file;
	long size;
	uint8_t *rom;
	struct gpu_unlock_fwsec fw;
	int ret;
	if (!rom_path || !info)
		return -EINVAL;
	file = fopen(rom_path, "rb");
	if (!file)
		return -ENOENT;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	rom = malloc((size_t)size);
	if (!rom) {
		fclose(file);
		return -ENOMEM;
	}
	if (fread(rom, 1, (size_t)size, file) != (size_t)size) {
		free(rom);
		fclose(file);
		return -EIO;
	}
	fclose(file);
	ret = gpu_unlock_vbios_fwsec(rom, (size_t)size, &fw);
	free(rom);
	if (ret)
		return ret;
	info->version = fw.version;
	info->image_off = fw.image_off;
	info->image_size = fw.image_size;
	info->data_off = fw.data_off;
	info->imem_size = fw.imem_size;
	info->dmem_size = fw.dmem_size;
	info->interface_off = fw.interface_off;
	return 0;
}

GD_EXPORT int gd_aes_decrypt_buffer(const uint8_t *key, uint8_t *data, size_t size)
{
	if (!key || !data)
		return -EINVAL;
	return gd_aes128_decrypt(key, data, size);
}

GD_EXPORT int gd_test_aes(void)
{
	return gd_aes128_selftest() ? 1 : 0;
}

GD_EXPORT int gd_test_wpr2(void)
{
	return gd_wpr2_selftest();
}

GD_EXPORT int gd_run_transaction(uint16_t device, uint32_t chip,
				 const char *rom_path, const char *asset_root, const char *trace_path, const char *code_path,
				 const char *booter_code_path, const uint8_t *key,
				 int threads, int verbose,
				 struct gd_result *out)
{
	struct gd_board board = {
		.device = device,
		.chip = chip,
	};
	struct gd_run_config config = {
		.asset_root = asset_root,
		.rom_path = rom_path,
		.verbose = verbose != 0,
		.code_path = code_path,
		.booter_code_path = booter_code_path,
	};
	if (key) {
		memcpy(config.key, key, sizeof(config.key));
		config.key_set = true;
	}
	struct gd_device *dev;
	struct pci_dev pdev;
	struct gu_device gpu;
	struct gpu_unlock_610_shared shared;
	uint64_t mask = 0xffffffffull;
	int ret;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	memset(&shared, 0, sizeof(shared));
	dev = gd_create(&board, &config);
	if (!dev) {
		out->driver_result = -ENODEV;
		out->device_error = -ENODEV;
		return -ENODEV;
	}
	memset(&pdev, 0, sizeof(pdev));
	pdev.dev.gd = dev;
	pdev.dev.dma_mask = &mask;
	pdev.dev.coherent_dma_mask = 0xffffffffull;
	pdev.vendor = 0x10de;
	pdev.device = device;
	pdev.current_state = 0; /* PCI_D0 */
	pdev.pm_cap = 0;
	pdev.refs = 1;
	gpu.pdev = &pdev;
	gpu.recipe = gu_card_find(device);
	if (!gpu.recipe) {
		out->driver_result = -ENODEV;
		out->device_error = -ENODEV;
		gd_destroy(dev);
		return -ENODEV;
	}
	/* Stage 1 runs the completion path synchronously; stage 2 turns the
	 * free-running engine threads on through the same device. */
	gd_set_engines(dev, threads != 0);

	ret = gpu_unlock_610_load_shared(&gpu, &shared);
	if (!ret)
		ret = gpu_unlock_610_unlock(&gpu, &shared, false);
	gpu_unlock_610_release_shared(&shared);
	gd_collect(dev, out);
	gd_trace_dump(dev, trace_path);
	out->driver_result = ret;
	gd_destroy(dev);
	return ret;
}
