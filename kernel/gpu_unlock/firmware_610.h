/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_FIRMWARE_610_H
#define GPU_UNLOCK_FIRMWARE_610_H

#include "prep_610.h"
#include "device.h"
#include "recipes.h"

struct firmware;

/* Immutable after preparation, including decoded metadata. Owns gsp and boot.
 * key is the family file set this slot was claimed for. */
struct gpu_unlock_610_firmware {
	const struct gu_fw_desc *key;
	const struct firmware *gsp, *boot;
	struct gpu_unlock_span image_span, sig_span;
	struct gpu_unlock_boot_desc boot_desc;
	struct gpu_unlock_radix_plan radix;
	bool prepared;
	int error;
};

/* A loaded Booter image. Families that share a gu_booter share one slot. */
struct gpu_unlock_610_booter {
	const struct gu_booter *key;
	const struct firmware *fw;
	struct gpu_unlock_booter_hdr hdr;
};

#define GU_SHARED_BUNDLES 3
#define GU_SHARED_BOOTERS 3

/* One coordination round. Bundles and Booters are interned by descriptor
 * pointer, so families that share a Booter load it once. */
struct gpu_unlock_610_shared {
	struct gpu_unlock_610_firmware bundle[GU_SHARED_BUNDLES];
	struct gpu_unlock_610_booter booter[GU_SHARED_BOOTERS];
	const struct firmware *loader;
	const char *loader_name;
};

static inline const struct gpu_unlock_610_firmware *
gpu_unlock_610_bundle_find(const struct gpu_unlock_610_shared *shared,
			   const struct gu_fw_desc *fw)
{
	unsigned int i;

	if (!shared || !fw)
		return NULL;
	for (i = 0; i < GU_SHARED_BUNDLES; i++)
		if (shared->bundle[i].key == fw)
			return &shared->bundle[i];
	return NULL;
}

static inline const struct gpu_unlock_610_booter *
gpu_unlock_610_booter_find(const struct gpu_unlock_610_shared *shared,
			   const struct gu_booter *desc)
{
	unsigned int i;

	if (!shared || !desc)
		return NULL;
	for (i = 0; i < GU_SHARED_BOOTERS; i++)
		if (shared->booter[i].key == desc)
			return &shared->booter[i];
	return NULL;
}

/* Serial preparation only; no workers may run until all calls have returned.
 * Cache failures for this round without invalidating another family's assets.
 * A fresh round starts after release_shared(), including on resume. */
int gpu_unlock_610_load_shared(const struct gu_device *device, struct gpu_unlock_610_shared *shared);
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared);

#endif
