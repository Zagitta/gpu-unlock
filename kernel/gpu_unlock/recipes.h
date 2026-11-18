/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_RECIPES_H
#define GPU_UNLOCK_RECIPES_H
#include "payload.h"
#include "memory.h"

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

/* Permissions precede an aligned enable block; handover closes only the
 * temporary host permissions after memory finalization and scrub. */
struct gu_ecc_recipe {
	u32 carveout_reg;
	u8 min_subrevision; /* silicon prerequisite for the enable recipe */
	struct gu_write_table prepare, enabled, handover;
};

struct gu_feature_layout {
	u32 plm, ss0, ss1, readout;
	u32 ecc, ecc_readout;
};

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

/* Fuse block of one generation, read for the boot snapshot. The per-GPC
 * arrays hold absolute BAR0 addresses, one entry per GPC of the full chip. */
struct gu_fuse_layout {
	u32 base;
	u8 addr_bits; /* FUSEADDR and IFF_RECORD field width */
	u8 tpc_bits;  /* TPC mask bits per GPC */
	u8 gpcs;
	bool ampere;  /* IFF_SW_FUSING, DISABLE_SW_OVERRIDE_STATUS and TPC_FLOORSWEEP_PLM exist */
	const u32 *tpc_disable, *tpc_defect;
};

/* Where the memory controllers latch the VRAM temperatures, see vram.c. */
enum gu_vram {
	GU_VRAM_NONE,
	GU_VRAM_GDDR, /* per chip */
	GU_VRAM_HBM,  /* per stack */
};

struct gu_rom_recipe {
	bool ecc;
	u16 p8_min_mhz; /* optional P8 override; zero leaves the board policy alone */
	u16 gpc_offset_mhz; /* enable offsets only when the factory range is zero */
	u8 min_subrevision; /* silicon prerequisite for this override bundle */
};

struct gu_family {
	const char *name;
	enum gu_vram vram;
	const struct gu_fuse_layout *fuse;
	/* selectors: optional feature overrides, first in the Booter sequence.
	 * privileged: all ordinary configuration through Booter, before ECC.
	 * relink: repeated before retraining, including after NVIDIA's devinit.
	 * bar1: coupled to the PCI resource resize.
	 * handover: restore temporary state and lock what an FLR would reset. */
	struct gu_write_table selectors, privileged, relink, bar1, handover;
	u8 link_speed; /* PCIe generation to retrain to at handover, 0 for none */
	const struct gu_fw_desc *fw;
	const struct gu_booter *booter;
	const char *loader; /* NULL when this family has no generic loader */
	u8 fwsec_version;
	bool frts; /* stock GSP-FRTS exists for this chip */
	bool pmu_riscv; /* PMU has the RISC-V/Falcon core selector */
	const struct gu_feature_layout *feat;
	const struct gu_ecc_recipe *ecc; /* NULL until the family supports ECC selection */
	struct gu_rom_recipe rom;
	bool p2p; /* verify the secure-option read after the privileged writes */
	enum gu_memory_kind memory;
	bool scrub_via_dmactl; /* post-reset scrub status in DMACTL, not HWCFG2 */
};
struct gu_card {
	u16 device;
	u32 chip; /* PMC_BOOT_0 architecture + implementation */
	const struct gu_family *family;
	struct gu_write_table privileged; /* final suffix; geometry prevents relaunch */
	/* Optional TGP range expansion; zero preserves the board's bound. */
	u32 power_min_mw, power_max_mw;
};
/* All writes verify a nonzero mask. RMW keeps the bits selected by kept. */
#define GU_WRITE(reg, val) \
	{ .addr = (reg), .value = (val), .check_mask = ~0u }
#define GU_WRITE_MASK(reg, val, kept, mask) \
	{ .addr = (reg), .value = (val), .keep = (kept), .check_mask = (mask) }
#define GU_OPTIONAL(reg, val) \
	{ .addr = (reg), .value = (val), .check_mask = ~0u, .optional = true }
#define GU_OPTIONAL_RMW(reg, val, kept) \
	{ .addr = (reg), .value = (val), .keep = (kept), .check_mask = ~0u, .optional = true }
#define GU_WRITE_TABLE(t) { .entries = (t), .count = sizeof(t) / sizeof((t)[0]) }

const struct gu_card *gu_card_find(u16 device);

/* Allocates the complete privileged sequence; caller owns *writes. */
int gu_recipe_prepare(const struct gu_card *card, bool ecc_enabled,
		      struct gu_reg_write **writes, size_t *count);
#endif
