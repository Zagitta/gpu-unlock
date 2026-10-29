/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_RECIPES_H
#define GPU_UNLOCK_RECIPES_H
#include "payload.h"

#define GU_FW_GSP_GA10X    "gpu-unlock/610.43.03/gsp_ga10x.bin"
#define GU_FW_BOOT_GA102   "gpu-unlock/610.43.03/gsp_boot_ga102.bin"
#define GU_FW_DESC_GA102   "gpu-unlock/610.43.03/gsp_boot_desc_ga102.bin"
#define GU_FW_GSP_GA100    "gpu-unlock/610.43.03/gsp_tu10x.bin"
#define GU_FW_BOOT_GA100   "gpu-unlock/610.43.03/gsp_boot_ga100.bin"
#define GU_FW_DESC_GA100   "gpu-unlock/610.43.03/gsp_boot_desc_ga100.bin"
#define GU_FW_LOADER       "gpu-unlock/610.43.03/falcon_loader.bin"
#define GU_FW_BOOTER_GA100 "gpu-unlock/610.43.03/booter_load_ga100.bin"
#define GU_FW_BOOTER_GA102 "gpu-unlock/610.43.03/booter_load_ga102.bin"
#define GU_FW_BOOTER_AD102 "gpu-unlock/610.43.03/booter_load_ad102.bin"
#define GU_FW_BOOT_AD102   "gpu-unlock/610.43.03/gsp_boot_ad102.bin"
#define GU_FW_DESC_AD102   "gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin"
#define GU_FW_BOOTER_TU102 "gpu-unlock/610.43.03/booter_load_tu102.bin"
#define GU_FW_BOOT_TU102   "gpu-unlock/610.43.03/gsp_boot_tu102.bin"
#define GU_FW_DESC_TU102   "gpu-unlock/610.43.03/gsp_boot_desc_tu102.bin"
#define GU_FW_SEC2_BL_TU102 "gpu-unlock/610.43.03/sec2_bl_tu102.bin"

struct gu_write_table { const struct gu_reg_write *entries; size_t count; };

/* Header fields that differ between signed Booter images. The common checks
 * stay in the validator. */
struct gu_booter_expect {
	u32 image_size, num_sigs, sig_bytes;
	u32 os_data_off, os_data_size, app_code_size, patch_loc;
	u32 ucode_id;
};

enum gu_sec2_launch {
	GU_SEC2_DIRECT, /* IMEM copy. FWSEC uses the generic Falcon loader. */
	GU_SEC2_HS,
};

/* One signed SEC2 Booter image. The family selects the Booter, and the Booter
 * selects its payload: the payload's offsets only mean something for the DMEM
 * frame layout of this image. Families that share a pointer share one loaded
 * slot. */
struct gu_booter {
	const char *file;
	struct gu_booter_expect expect;
	enum gu_sec2_launch launch; /* how this image is started */
	const struct gu_payload *payload;
};

/* Immutable file set for one GSP image. Families that share a pointer share
 * one loaded bundle. */
struct gu_fw_desc {
	const char *gsp, *boot, *desc, *signature_section;
	u32 boot_size;
};

struct gu_family {
	const char *name;
	struct gu_write_table privileged, host;
	const struct gu_fw_desc *fw;
	const struct gu_booter *booter;
	const char *loader; /* NULL when this family has no generic loader */
	u8 fwsec_version;
	u32 feat_plm, feat_ss0, feat_ss1;
	bool turing_layout; /* VBIOS FB-range register and no MMU lock */
	/* The Booter's own privileged writes can set the host table's selectors, so
	 * direct mode never opens a PLM. The Booter runs heavy-secure (L3), which the
	 * power-on FEAT PLM already lets write them. */
	bool direct_selectors;
};
struct gu_card {
	u16 device;
	u32 chip; /* PMC_BOOT_0 architecture + implementation */
	const struct gu_family *family;
};
#define GU_WRITE_TABLE(t) { .entries = (t), .count = sizeof(t) / sizeof((t)[0]) }

const struct gu_card *gu_card_find(u16 device);
#endif
