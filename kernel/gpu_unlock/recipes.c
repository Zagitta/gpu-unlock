// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "recipes.h"
#include "registers.h"

/* GA102 six-write call table. Offsets are relative to the signature image. */
static const struct gu_payload_word ga102_words[] = {
	{ 0x1100, 0x00000007 },
	{ 0xf93c, 0x00006140 },	/* signature frame r3 */
	{ 0xf950, 0x00001110 },	/* signature ret -> write 1 */
	{ 0xf954, 0x00006140 },
	{ 0xf968, 0x00001110 },	/* write 1 ret -> write 2 */
	{ 0xf96c, 0x00006140 },
	{ 0xf980, 0x00001110 },	/* write 2 ret -> write 3 */
	{ 0xf984, 0x00006140 },
	{ 0xf998, 0x00001110 },	/* write 3 ret -> write 4 */
	{ 0xf99c, 0x00006140 },
	{ 0xf9b0, 0x00001110 },	/* write 4 ret -> write 5 */
	{ 0xf9b4, 0x00006140 },
	{ 0xf9c8, 0x00001110 },	/* write 5 ret -> write 6 */
	{ 0xf9cc, 0x00006140 },	/* write 6 frame r3 */
	{ 0xf9d4, 0x00000000 },	/* write 6 epilogue r1 (unused) */
	{ 0xf9d8, 0x00000000 },	/* write 6 epilogue r0 (unused) */
	{ 0xf9e0, 0x000085d0 },	/* write 6 ret -> plain ret */
	{ 0xf9e4, 0x00008445 },	/* -> booter release/completion, sp=0xffe8 */
};
static const struct gu_payload_slot ga102_slots[] = {
	{ 0xf944, 0xf948 },
	{ 0xf95c, 0xf960 },
	{ 0xf974, 0xf978 },
	{ 0xf98c, 0xf990 },
	{ 0xf9a4, 0xf9a8 },
	{ 0xf9bc, 0xf9c0 },
};

static const struct gu_payload ga102_payload = {
	.name = "ga102",
	.size = 0xf9f4,
	.slots = ga102_slots,
	.writes = sizeof(ga102_slots) / sizeof(ga102_slots[0]),
	.words = ga102_words,
	.word_count = sizeof(ga102_words) / sizeof(ga102_words[0]),
	.completion = GU_PAYLOAD_HALT_RESET_PLM_ZERO,
};

/* GA100 six-write chain. The first two writes use the re-entrant 0x10b9
 * engine and the proven refire cleanup, which releases the SE mutex while the
 * booter's DIC entry is still live. The 0xccb return is then redirected to a
 * -0x1344 stack pivot (0x7934), a +0x38 pivot (0x717), and the 0x8fe reload
 * gadget, which seeds r3=0x6340 and the third write's parameters from a lower
 * frame at 0xED14. The last four writes land there and exit through 0x7f2f.
 * Slots 3 and 4 move to 0xE504/0xE508 and 0xE520/0xE524 so they do not overlap
 * the first pair's standard stack frames. The image is otherwise zero, so the
 * 0xF800 signature DMA overwrites the booter's per-boot TRNG guard at 0x6340
 * (image 0x5B40) and every saved canary copy with the same zero; the guard
 * checks then compare 0 == 0. On hardware 0x6340 is a random canary before
 * that DMA, not zero. */
static const struct gu_payload_word ga100_words[] = {
	{ 0x1100, 0x00000007 },
	{ 0xe4bc, 0x00000717 },	/* -0x1344 pivot lands here; B pivot */
	{ 0xe4d0, 0x00006340 },	/* 0x717 r3 pop */
	{ 0xe4f4, 0x000008fe },	/* 0x717 ret -> 0x8fe reload */
	{ 0xe4fc, 0x00006340 },	/* reload r3 */
	{ 0xe514, 0x000010b9 },	/* reload ret -> write 3 */
	{ 0xe518, 0x00006340 },	/* write 3 frame r3 */
	{ 0xe52c, 0x000010b9 },	/* write 3 ret -> write 4 */
	{ 0xe530, 0x00006340 },	/* write 4 frame r3 */
	{ 0xe544, 0x000010b9 },	/* write 4 ret -> write 5 */
	{ 0xe548, 0x00006340 },	/* write 5 frame r3 */
	{ 0xe55c, 0x000010b9 },	/* write 5 ret -> write 6 */
	{ 0xe560, 0x00006340 },	/* write 6 frame r3 */
	{ 0xe574, 0x00007f2f },	/* write 6 exit */
	{ 0xf748, 0x00006340 },	/* write 1 frame r3 */
	{ 0xf75c, 0x000010b9 },	/* write 1 ret -> write 2 */
	{ 0xf760, 0x00006340 },	/* write 2 frame r3 */
	{ 0xf774, 0x000010b9 },	/* write 2 ret -> write 3 */
	{ 0xf778, 0x00006340 },	/* frame 3 r3 */
	{ 0xf78c, 0x0000815a },	/* refire cleanup entry */
	{ 0xf790, 0x00008e18 },	/* cleanup block address */
	{ 0xf794, 0x00000000 },
	{ 0xf798, 0x0000815a },
	{ 0xf79c, 0x00000000 },
	{ 0xf7a0, 0x00000000 },
	{ 0xf7a4, 0x00001fbd },
	{ 0xf7b0, 0x0000ffbc },
	{ 0xf7b8, 0x0000582d },
	{ 0xf7c4, 0x00000000 },
	{ 0xf7c8, 0x00000cbd },
	{ 0xf7d8, 0x00000003 },
	{ 0xf7e0, 0x00001fbd },
	{ 0xf7f4, 0x00000ccb },
	{ 0xf7f8, 0x00007934 },	/* cleanup return -> -0x1344 pivot */
};
static const struct gu_payload_slot ga100_slots[] = {
	{ 0xf750, 0xf754 },
	{ 0xf768, 0xf76c },
	{ 0xe504, 0xe508 },
	{ 0xe520, 0xe524 },
	{ 0xe538, 0xe53c },
	{ 0xe550, 0xe554 },
};
static const struct gu_payload ga100_payload = {
	.name = "ga100",
	.size = 0xf800,
	.slots = ga100_slots,
	.writes = sizeof(ga100_slots) / sizeof(ga100_slots[0]),
	.words = ga100_words,
	.word_count = sizeof(ga100_words) / sizeof(ga100_words[0]),
	.completion = GU_PAYLOAD_HALT_RESET_PLM,
};

/* AD102 uses the same six write frames as GA102, with relocated gadgets.
 * The final frame's saved r0/r1 are unused by the normal completion path;
 * leave them zero instead of baking a FEAT permission opening into them. */
static const struct gu_payload_slot ad102_slots[] = {
	{ 0xf944, 0xf948 },
	{ 0xf95c, 0xf960 },
	{ 0xf974, 0xf978 },
	{ 0xf98c, 0xf990 },
	{ 0xf9a4, 0xf9a8 },
	{ 0xf9bc, 0xf9c0 },
};
static const struct gu_payload_word ad102_words[] = {
	{ 0x1100, 0x00000007 },
	{ 0xf93c, 0x00005f50 },
	{ 0xf950, 0x0000257e },
	{ 0xf954, 0x00005f50 },
	{ 0xf968, 0x0000257e },
	{ 0xf96c, 0x00005f50 },
	{ 0xf980, 0x0000257e },
	{ 0xf984, 0x00005f50 },
	{ 0xf998, 0x0000257e },
	{ 0xf99c, 0x00005f50 },
	{ 0xf9b0, 0x0000257e },
	{ 0xf9b4, 0x00005f50 },
	{ 0xf9c8, 0x0000257e },
	{ 0xf9cc, 0x00005f50 },
	{ 0xf9e0, 0x00007b0a },
	{ 0xf9e4, 0x0000797f },
};
static const struct gu_payload ad102_payload = {
	.name = "ad102",
	.size = 0xf9f4,
	.slots = ad102_slots,
	.writes = sizeof(ad102_slots) / sizeof(ad102_slots[0]),
	.words = ad102_words,
	.word_count = sizeof(ad102_words) / sizeof(ad102_words[0]),
	.completion = GU_PAYLOAD_HALT_RESET_PLM_ZERO,
};

/* TU102 port of the refire chain.  The GA100/TU102 Booters are the same
 * code with functions moved; fwmap gives a one-for-one mapping for every
 * gadget used here: write entry 0x10b9, cleanup epilogue 0x815a -> 0x7f27,
 * bounce 0x1fbd -> 0x1f8b, CSB read entry 0x582d -> 0x653f, read epilogue
 * 0xcbd -> 0xcbd and release 0xccb -> 0xccb.  The guard moves 0x6340 ->
 * 0x6140.
 *
 * Six write frames exit through Turing's normal mutex release/completion.
 * The signature DMA clears the guard at 0x6140 and every saved copy, so the
 * guard checks compare 0 == 0. */
static const struct gu_payload_slot tu102_slots[] = {
	{ 0xf940, 0xf944 },
	{ 0xf958, 0xf95c },
	{ 0xf970, 0xf974 },
	{ 0xf988, 0xf98c },
	{ 0xf9a0, 0xf9a4 },
	{ 0xf9b8, 0xf9bc },
};
static const struct gu_payload_word tu102_words[] = {
	{ 0x1100, 0x00000007 },
	{ 0xf938, 0x00006140 },	/* signature frame r3 */
	{ 0xf94c, 0x000010b9 },	/* signature ret -> write 1 */
	{ 0xf950, 0x00006140 },
	{ 0xf964, 0x000010b9 },	/* write 1 ret -> write 2 */
	{ 0xf968, 0x00006140 },
	{ 0xf97c, 0x000010b9 },	/* write 2 ret -> write 3 */
	{ 0xf980, 0x00006140 },
	{ 0xf994, 0x000010b9 },	/* write 3 ret -> write 4 */
	{ 0xf998, 0x00006140 },
	{ 0xf9ac, 0x000010b9 },	/* write 4 ret -> write 5 */
	{ 0xf9b0, 0x00006140 },
	{ 0xf9c4, 0x000010b9 },	/* write 5 ret -> write 6 */
	{ 0xf9c8, 0x00006140 },	/* write 6 frame r3 */
	{ 0xf9d0, 0x00000000 },	/* write 6 epilogue r1 (unused) */
	{ 0xf9d4, 0x00000000 },	/* write 6 epilogue r0 (unused) */
	{ 0xf9dc, 0x0000802f },	/* write 6 ret -> plain ret */
	{ 0xf9e0, 0x00007ea4 },	/* -> booter release/completion, sp=0xffe4 */
};
static const struct gu_payload tu102_payload = {
	.name = "tu102",
	.size = 0xf9f0,
	.slots = tu102_slots,
	.writes = sizeof(tu102_slots) / sizeof(tu102_slots[0]),
	.words = tu102_words,
	.word_count = sizeof(tu102_words) / sizeof(tu102_words[0]),
	.completion = GU_PAYLOAD_HALT_RESET_PLM_ZERO,
};

/* Selectors are written at level 3; their default PLMs already admit SEC2.
 * Host access is read-only, so no FEAT permission opening is needed. */
static const struct gu_feature_layout ampere_features = {
	.plm = GU_FEAT_PLM_OFFSET,
	.ss0 = GU_FEAT_SS0_OFFSET, .ss1 = GU_FEAT_SS1_OFFSET,
	.readout = GU_FEAT_READOUT_1_OFFSET,
	.ecc = GU_FEAT_ECC_0_OFFSET, .ecc_readout = GU_FEAT_ECC_READOUT_OFFSET,
};
static const struct gu_feature_layout turing_features = {
	.plm = GU_TU102_FEAT_PLM_OFFSET,
	.ss0 = GU_TU102_FEAT_SS0_OFFSET, .ss1 = GU_TU102_FEAT_SS1_OFFSET,
	.readout = GU_TU102_FEAT_READOUT_OFFSET,
	.ecc = GU_TU102_FEAT_ECC_OFFSET,
	.ecc_readout = GU_TU102_FEAT_ECC_READOUT_OFFSET,
};

static const struct gu_reg_write ampere_selectors[] = {
	GU_WRITE(GU_FEAT_SS0_OFFSET, GU_SS0_UNLOCK),
	GU_WRITE(GU_FEAT_SS1_OFFSET, GU_SS1_UNLOCK),
};
static const struct gu_reg_write turing_selectors[] = {
	GU_WRITE(GU_TU102_FEAT_SS0_OFFSET, GU_SS0_UNLOCK),
	GU_WRITE(GU_TU102_FEAT_SS1_OFFSET, GU_SS1_UNLOCK),
};

/* Shared Ampere/Ada workaround. Install match/data before arming trap 30. */
static const struct gu_reg_write p2p_privileged[] = {
	GU_WRITE(GU_P2P_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_P2P_TRAP_PLM_OFFSET, 0xfffffb9f),
	GU_WRITE(GU_P2P_TRAP_MATCH_OFFSET, GU_P2P_SECURE_OFFSET),
	GU_WRITE(GU_P2P_TRAP_MASK_OFFSET, 0xfc000000),
	GU_WRITE(GU_P2P_TRAP_DATA1_OFFSET, 0),
	GU_WRITE(GU_P2P_TRAP_DATA2_OFFSET, 0),
	GU_WRITE(GU_P2P_TRAP_CFG_OFFSET, 0x3c060),
	GU_WRITE(GU_P2P_TRAP_ACTION_OFFSET, 0x11),
};

/* Only host memory bookkeeping and scrub need permission openings.
 * The actual mode writes execute at level 3 under the default permissions. */
static const struct gu_reg_write gddr_ecc_prepare[] = {
	GU_WRITE(GU_WPR_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_MMU_LOCK_CFG_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_GFW_FB_SIZE_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_MEMORY_RANGE_MIRROR_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_VPR_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_VPR_BOOT_HANDOFF_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_DISPLAY_VPR_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_NISO_SCRUB_PLM_OFFSET, 0xffffffff),
};
static const struct gu_reg_write turing_ecc_prepare[] = {
	/* Turing shares one gate for local range, MMU_LOCK and VPR. */
	GU_WRITE(GU_TU102_MMU_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_MEMORY_RANGE_MIRROR_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_VPR_BOOT_HANDOFF_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_DISPLAY_VPR_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_NISO_SCRUB_PLM_OFFSET, 0xffffffff),
};

/* DRAM and L2 ECC must change together in the final launch. The feature
 * override address is selected from the family's layout when copied. */
static const struct gu_reg_write gddr_ecc_enable[] = {
	GU_WRITE_MASK(GU_FEAT_ECC_0_OFFSET, 0x99000, ~0x99000u, 0x99000),
	GU_WRITE_MASK(GU_FB_L2_ECC_CONFIG_OFFSET, 1, ~1u, 1),
	GU_WRITE_MASK(GU_L2_DATA_CONFIG_OFFSET, 0x11, ~0x11u, 0x11),
	GU_WRITE_MASK(GU_L2_INPUT_QUEUE_CONFIG_OFFSET, 1, ~1u, 1),
};

static const struct gu_reg_write ga100_hbm_access[] = {
	GU_WRITE(GU_FBPA_CONFIG_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_FBPA_HBMPLL_PLM_OFFSET, 0xffffffff),
};

/* cmpunlocker 2d10f891, sec2-postbl-plm-ss-cfg.patch, plmTable, then the
 * PLMs pcie-gen2.patch and ecc-enable.patch open. Ordinary configuration follows here;
 * PCIe retraining and BAR1 resize retain their own write sequences. cmpunlocker
 * carries on when the later PLMs do not read back, so they are optional here.
 */
static const struct gu_reg_write ga100_privileged[] = {
	GU_WRITE(GU_FBPA_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_WPR_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_XVE_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_XVE_B_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_XVE_C_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_FEAT2_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_OPT_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_RECONFIG_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_PJTAG_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_PJTAG_SEC_PLM_OFFSET, 0xffffffff),
	GU_OPTIONAL(GU_XP3G_PLM(0),          	0xffffffff),
	GU_OPTIONAL(GU_XP3G_PLM(1),          	0xffffffff),
	GU_OPTIONAL(GU_XP3G_PLM(2),          	0xffffffff),
	GU_OPTIONAL(GU_XP3G_PLM(3),          	0xffffffff),
	GU_OPTIONAL(GU_XVE_D_PLM(0),         	0xffffffff),
	GU_OPTIONAL(GU_XVE_D_PLM(1),         	0xffffffff),
	GU_OPTIONAL(GU_XVE_D_PLM(2),         	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xd0),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xd4),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xdc),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xe0),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xe4),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xe8),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xec),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xf0),  	0xffffffff),
	GU_OPTIONAL(GU_OPT_BLOCK_PLM(0xf4),  	0xffffffff),
	GU_OPTIONAL(GU_FEAT_ECC_PLM_OFFSET,  	0xffffffff),
	GU_OPTIONAL(GU_MMU_LOCK_CFG_PLM_OFFSET,	0xffffffff),
	GU_OPTIONAL(GU_WPR_PLM_C0_OFFSET,    	0xffffffff),
	GU_OPTIONAL(GU_WPR_PLM_D0_OFFSET,    	0xffffffff),
	GU_WRITE(GU_FBPA_CONFIG_PLM_OFFSET, 0xffffffff),
	GU_WRITE(GU_FBPA_HBMPLL_PLM_OFFSET, 0xffffffff),
	/* SM-RECONFIG, cmpunlocker sec2-postbl-plm-ss-cfg.patch. */
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(0), 0x000000ff),
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(1), 0x000000ff),
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(2), 0x000000ff),
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(3), 0x000000ff),
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(4), 0x000000ff),
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(5), 0x000000ff),
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(6), 0x000000ff),
	GU_OPTIONAL(GU_SM_RECONFIG_OVR(7), 0x000000ff),
	/* WPR1 must stay down for every following Booter launch. */
	GU_WRITE(GU_WPR1_ADDR_HI_OFFSET, 0),
	GU_WRITE(GU_WPR1_ADDR_LO_OFFSET, 0x1ffffe00),
};

/* Fused defaults leave ECC disabled; only override them when EEN requests it.
 * MASTER_EN is read-only; change only SIDEBAND_EN in FBPA_ECC_CONTROL.
 * The three feature overrides and sideband setting must share a launch:
 * restarting Booter between them fails its WPR range validation. */
static const struct gu_reg_write ga100_ecc_enable[] = {
	GU_WRITE_MASK(GU_FEAT_ECC_0_OFFSET, 0x999999, 0, 0x111111),
	GU_WRITE_MASK(GU_FEAT_ECC_1_OFFSET, 0x3ffebf, 0, 0x155415),
	GU_WRITE_MASK(GU_FEAT_ECC_2_OFFSET,      0xf, 0, 0x5),
	GU_WRITE_MASK(GU_FBPA_ECC_CONTROL_OFFSET, 0x40, ~0x40u, 0x40),
	GU_WRITE_MASK(GU_FBPA_HBM_CFG0_OFFSET,   0x200, ~0x200u, 0x200),
	GU_WRITE_MASK(GU_DRAM_MODE_4_OFFSET,       0x3, ~0x3u, 0x3),
	GU_WRITE_MASK(GU_FB_L2_ECC_CONFIG_OFFSET,  0x1, ~0x1u, 0x1),
	GU_WRITE_MASK(GU_L2_DATA_CONFIG_OFFSET,    0x1, ~0x1u, 0x1),
	GU_WRITE_MASK(GU_L2_INPUT_QUEUE_CONFIG_OFFSET, 0x1, ~0x1u, 0x1),
	/* Post-ECO GA100 repurposes SM_TTU as the poison-disable signal.
	 * Select its feature override and clear the inverted disable bit.
	 * The old GLOBAL_POISON_DIS fuse is ineffective on these revisions. */
	GU_WRITE_MASK(GU_FEAT_POISON_CONTROL_OFFSET, 0x02000000, ~0x03000000u, 0x03000000),
	GU_WRITE(GU_NISO_SCRUB_PLM_OFFSET, 0xffffffff),
};

static const struct gu_ecc_recipe ga100_ecc = {
	.carveout_reg = GU_INFOROM_CARVEOUT_OFFSET,
	.min_subrevision = 1, /* GA100 A01P: poison ECO */
	.enabled = GU_WRITE_TABLE(ga100_ecc_enable),
};

/* CMP 170HX memory geometry, cmpunlocker common/constants.yaml. The range
 * register decodes to 32 << 31 = 64 GiB and 40 << 30 = 40 GiB. An FLR sets
 * both back to stock, so the handover is without FLR. */
static const struct gu_reg_write cmp170hx_8gb_geometry[] = {
	GU_WRITE(GU_FBPA_CFG1_OFFSET, 0x02779000),
	GU_WRITE(GU_LOCAL_MEMORY_RANGE_OFFSET, 0x0000020b),
};
static const struct gu_reg_write cmp170hx_10gb_geometry[] = {
	GU_WRITE(GU_FBPA_CFG1_OFFSET, 0x02669000),
	GU_WRITE(GU_LOCAL_MEMORY_RANGE_OFFSET, 0x0000028a),
};

/* Close temporary host permissions after the last dependent host write.
 * All-ones PLMs legitimately look absent to the host, so locks are optional. */
#define GU_HOST_LOCK_OPEN(plm) GU_OPTIONAL_RMW(plm, 0, ~GU_PLM_HOST_WRITE)

/* Post-FLR values measured on CMP 170HX. WPR1 is restored by SEC2; WPR2
 * is disabled by the last payload slot. These ranges use separate gates. */
static const struct gu_reg_write ga100_handover[] = {
	GU_OPTIONAL(GU_WPR_ADDR_HI_08_OFFSET,   0x00000000),
	GU_OPTIONAL(GU_WPR_ADDR_LO_04_OFFSET,   0x1ffff000),
	GU_OPTIONAL(GU_MMU_LOCK_ADDR_HI_OFFSET, 0x00000000),
	GU_OPTIONAL(GU_MMU_LOCK_ADDR_LO_OFFSET, 0x1ffffff0),
	GU_OPTIONAL(GU_WPR_CTL_0C_OFFSET,       0x00000028),
	GU_HOST_LOCK_OPEN(GU_FBPA_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_XVE_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_OPT_BLOCK_PLM(0xd0)),
	GU_HOST_LOCK_OPEN(GU_OPT_BLOCK_PLM(0xdc)),
	GU_HOST_LOCK_OPEN(GU_WPR_PLM_C0_OFFSET),
	GU_HOST_LOCK_OPEN(GU_WPR_PLM_D0_OFFSET),
	GU_HOST_LOCK_OPEN(GU_MMU_LOCK_CFG_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_WPR_PLM_OFFSET),
};
static const struct gu_reg_write gddr_ecc_handover[] = {
	GU_HOST_LOCK_OPEN(GU_DISPLAY_VPR_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_VPR_BOOT_HANDOFF_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_VPR_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_MEMORY_RANGE_MIRROR_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_GFW_FB_SIZE_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_MMU_LOCK_CFG_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_WPR_PLM_OFFSET),
};
static const struct gu_reg_write turing_ecc_handover[] = {
	GU_HOST_LOCK_OPEN(GU_DISPLAY_VPR_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_VPR_BOOT_HANDOFF_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_MEMORY_RANGE_MIRROR_PLM_OFFSET),
	GU_HOST_LOCK_OPEN(GU_TU102_MMU_PLM_OFFSET),
};

static const struct gu_ecc_recipe gddr_ecc = {
	.carveout_reg = GU_INFOROM_CARVEOUT_OFFSET,
	.prepare = GU_WRITE_TABLE(gddr_ecc_prepare),
	.enabled = GU_WRITE_TABLE(gddr_ecc_enable),
	.handover = GU_WRITE_TABLE(gddr_ecc_handover),
};
static const struct gu_ecc_recipe turing_ecc = {
	.carveout_reg = GU_INFOROM_CARVEOUT_OFFSET,
	.prepare = GU_WRITE_TABLE(turing_ecc_prepare),
	.enabled = GU_WRITE_TABLE(gddr_ecc_enable),
	.handover = GU_WRITE_TABLE(turing_ecc_handover),
};

/* cmpunlocker 2d10f891 bar1-resize-unlock.patch. These advertise BAR1 sizes
 * up to 64 GiB and switch the decoder to 64 GiB at once, so gu_pcie_bar1_resize
 * must follow them. */
static const struct gu_reg_write ga100_bar1[] = {
	GU_OPTIONAL(GU_XVE_BAR1_CYA_OFFSET,   0x00000030),
	GU_OPTIONAL(GU_XVE_BAR1_REBAR_OFFSET, 0x8000000a),
};

/* PCIe Gen2 policy, cmpunlocker 2d10f891 pcie-gen2.patch as distilled by the
 * driverless refire chain that confirmed LnkCap2 0x2 -> 0x6 on hardware.
 * The stock driver's devinit reprograms the XVE strap shadows on every boot,
 * so the policy is applied before the initial retrain and again from the
 * relink path after devinit, immediately before the link retrain. Only the
 * XP3G override registers take a host value; the OPT and
 * XVE strap mirrors keep their fuse value (0x82057c reads 1, 0x8c1c0 bit 21
 * reads 0), so every entry is optional and pcie.c judges the result by
 * LnkCap2, not by these readbacks. */
static const struct gu_reg_write ga100_relink[] = {
	GU_OPTIONAL(GU_OPT_PCIE_GEN23_OFFSET,  0x00000000),
	GU_OPTIONAL(GU_OPT_PCIE_MAGIC_OFFSET,  0x00200000),
	GU_OPTIONAL(GU_XP3G_VAL(0),            0x00000000),
	GU_OPTIONAL(GU_XP3G_OVR(0),            0x00000001),
	GU_OPTIONAL(GU_XP3G_VAL(3),            0x00200000),
	GU_OPTIONAL(GU_XP3G_OVR(3),            0x00000004),
	GU_OPTIONAL_RMW(GU_XVE_VSEC_HIERARCHY_OFFSET, 0x00000001, ~0x1001u),
	GU_OPTIONAL_RMW(GU_XVE_PRIV_MISC_1_OFFSET, (1u << 11) | (1u << 13),
			~((1u << 11) | (1u << 12) | (1u << 13) | (1u << 14))),
	GU_OPTIONAL_RMW(GU_XP_CYA_0_OFFSET,         0x00000000, ~(1u << 2)),
	GU_OPTIONAL_RMW(GU_XP_LINK_CONFIG_0_OFFSET, 2u << 18,   ~(3u << 18)),
	GU_OPTIONAL(GU_XP_PL_LINK_RATE_OFFSET,      0x00240036),
	GU_OPTIONAL_RMW(GU_XVE_LINK_CTRL_2_OFFSET,  0x00000002, ~0x0fu),
	GU_OPTIONAL(GU_XVE_OVERRIDE_OFFSET,         0x00000006),
};

static const struct gu_booter sec2_ga100 = {
	.file = GU_FW_BOOTER_GA100,
	.launch = GU_SEC2_DIRECT,
	.payload = &ga100_payload,
	.expect = {
		.image_size = 60160,
		.num_sigs = 1,
		.sig_bytes = 384,
		.os_data_off = 34560,
		.os_data_size = 25600,
		.app_code_size = 34304,
		.patch_loc = 35072,
		.ucode_id = 3,
	},
};
/* The GA10x SEC2 Booter. Its signature patch location is 0x8a10. */
static const struct gu_booter sec2_ga102 = {
	.file = GU_FW_BOOTER_GA102,
	.launch = GU_SEC2_HS,
	.payload = &ga102_payload,
	.expect = {
		.image_size = 60416,
		.num_sigs = 2,
		.sig_bytes = 768,
		.os_data_off = 35328,
		.os_data_size = 25088,
		.app_code_size = 35072,
		.patch_loc = 35344,
		.ucode_id = 3,
	},
};
static const struct gu_booter sec2_ad102 = {
	.file = GU_FW_BOOTER_AD102,
	.launch = GU_SEC2_HS,
	.payload = &ad102_payload,
	.expect = {
		.image_size = 0xde00,
		.num_sigs = 2,
		.sig_bytes = 768,
		.os_data_off = 0x7e00,
		.os_data_size = 0x6000,
		.app_code_size = 0x7d00,
		.patch_loc = 0x7e10,
		.ucode_id = 3,
	},
};
static const struct gu_booter sec2_tu102 = {
	.file = GU_FW_BOOTER_TU102,
	.launch = GU_SEC2_DIRECT,
	.payload = &tu102_payload,
	.expect = {
		.image_size = 59136,
		.num_sigs = 1,
		.sig_bytes = 16,
		.os_data_off = 34048,
		.os_data_size = 25088,
		.app_code_size = 33792,
		.patch_loc = 34560,
		.ucode_id = 13,
	},
};

static const struct gu_fw_desc ga100_fw = {
	.gsp = GU_FW_GSP_GA100,
	.boot = GU_FW_BOOT_GA100,
	.desc = GU_FW_DESC_GA100,
	.signature_section = ".fwsignature_ga100",
	.boot_size = 4096,
};
static const struct gu_fw_desc ga10x_fw = {
	.gsp = GU_FW_GSP_GA10X,
	.boot = GU_FW_BOOT_GA102,
	.desc = GU_FW_DESC_GA102,
	.signature_section = ".fwsignature_ga10x",
	.boot_size = 24576,
};
static const struct gu_fw_desc ad102_fw = {
	.gsp = GU_FW_GSP_GA10X,
	.boot = GU_FW_BOOT_AD102,
	.desc = GU_FW_DESC_AD102,
	.signature_section = ".fwsignature_ad10x",
	.boot_size = 36864,
};
static const struct gu_fw_desc tu102_fw = {
	.gsp = GU_FW_GSP_GA100,
	.boot = GU_FW_BOOT_TU102,
	.desc = GU_FW_DESC_TU102,
	.signature_section = ".fwsignature_tu10x",
	.boot_size = 4096,
};

/* Per-GPC OPT_TPC_GPC disable and defective masks. GA10x uses the first seven
 * GA100 entries. */
static const u32 tu102_tpc_disable[] = {
	0x21760, 0x21764, 0x21768, 0x2176c, 0x21770, 0x21774,
};
static const u32 tu102_tpc_defect[] = {
	0x215c8, 0x21508, 0x2150c, 0x21514, 0x21564, 0x215ac,
};
static const u32 ga100_tpc_disable[] = {
	0x820760, 0x820764, 0x820768, 0x82076c, 0x820770, 0x820774, 0x820778, 0x82077c,
};
static const u32 ga100_tpc_defect[] = {
	0x8205c8, 0x820508, 0x82050c, 0x820514, 0x820564, 0x8205ac, 0x820b84, 0x820b80,
};
static const u32 ad102_tpc_disable[] = {
	0x820760, 0x820764, 0x820768, 0x82076c, 0x820770, 0x820774, 0x820778, 0x82077c,
	0x820780, 0x820784, 0x820ba0, 0x820ba4,
};
static const u32 ad102_tpc_defect[] = {
	0x8205c8, 0x820508, 0x82050c, 0x820514, 0x820564, 0x8205ac, 0x820b84, 0x8210a0,
	0x8210a4, 0x8210a8, 0x8210ac, 0x8210b0,
};

static const struct gu_fuse_layout tu102_fuse = {
	.base = 0x21000, .addr_bits = 8, .tpc_bits = 6, .gpcs = 6,
	.tpc_disable = tu102_tpc_disable, .tpc_defect = tu102_tpc_defect,
};
static const struct gu_fuse_layout ga100_fuse = {
	.base = 0x820000, .addr_bits = 9, .tpc_bits = 8, .gpcs = 8, .ampere = true,
	.tpc_disable = ga100_tpc_disable, .tpc_defect = ga100_tpc_defect,
};
static const struct gu_fuse_layout ga10x_fuse = {
	.base = 0x820000, .addr_bits = 10, .tpc_bits = 6, .gpcs = 7, .ampere = true,
	.tpc_disable = ga100_tpc_disable, .tpc_defect = ga100_tpc_defect,
};
static const struct gu_fuse_layout ad10x_fuse = {
	.base = 0x820000, .addr_bits = 10, .tpc_bits = 6, .gpcs = 12, .ampere = true,
	.tpc_disable = ad102_tpc_disable, .tpc_defect = ad102_tpc_defect,
};

static const struct gu_family ga100 = {
	.ecc = &ga100_ecc,
	.name = "GA100",
	.memory = GU_MEMORY_RANGE,
	.vram = GU_VRAM_HBM,
	.fuse = &ga100_fuse,
	.selectors = GU_WRITE_TABLE(ampere_selectors),
	.privileged = GU_WRITE_TABLE(ga100_privileged),
	.rom = { .ecc = true, .p8_min_mhz = 135, .gpc_offset_mhz = 1000, .min_subrevision = 1 },
	.p2p = true,
	.relink = GU_WRITE_TABLE(ga100_relink),
	.bar1 = GU_WRITE_TABLE(ga100_bar1),
	.handover = GU_WRITE_TABLE(ga100_handover),
	.link_speed = 2,
	.fw = &ga100_fw,
	.booter = &sec2_ga100,
	.loader = GU_FW_LOADER,
	.fwsec_version = 2,
	.frts = false,
	.feat = &ampere_features,
	.scrub_via_dmactl = true,
};
/* A100 needs only host access for the existing HBM governor. */
static const struct gu_family ga100_hbm = {
	.name = "A100 80GB PCIe",
	.rom = { .p8_min_mhz = 135, .gpc_offset_mhz = 1000 },
	.memory = GU_MEMORY_RANGE,
	.vram = GU_VRAM_HBM,
	.fuse = &ga100_fuse,
	.privileged = GU_WRITE_TABLE(ga100_hbm_access),
	.fw = &ga100_fw,
	.booter = &sec2_ga100,
	.loader = GU_FW_LOADER,
	.fwsec_version = 2,
	.feat = &ampere_features,
	.scrub_via_dmactl = true,
};
static const struct gu_family ga10x = {
	.memory = GU_MEMORY_GDDR,
	.pmu_riscv = true,
	.ecc = &gddr_ecc,
	.rom = { .ecc = true },
	.name = "GA102/GA103/GA104/GA106",
	.vram = GU_VRAM_GDDR,
	.fuse = &ga10x_fuse,
	.selectors = GU_WRITE_TABLE(ampere_selectors),
	.p2p = true,
	.fw = &ga10x_fw,
	.booter = &sec2_ga102,
	.loader = NULL,
	.fwsec_version = 3,
	.frts = true,
	.feat = &ampere_features,
};
static const struct gu_family ad10x = {
	.memory = GU_MEMORY_GDDR,
	.pmu_riscv = true,
	.ecc = &gddr_ecc,
	.rom = { .ecc = true },
	.name = "AD102/AD103/AD104/AD106/AD107",
	.vram = GU_VRAM_GDDR,
	.fuse = &ad10x_fuse,
	.selectors = GU_WRITE_TABLE(ampere_selectors),
	.p2p = true,
	.fw = &ad102_fw,
	.booter = &sec2_ad102,
	.loader = NULL,
	.fwsec_version = 3,
	.frts = true,
	.feat = &ampere_features,
};
static const struct gu_family tu102 = {
	.ecc = &turing_ecc,
	.rom = { .ecc = true },
	.name = "TU102/TU104/TU106",
	.vram = GU_VRAM_GDDR,
	.fuse = &tu102_fuse,
	.selectors = GU_WRITE_TABLE(turing_selectors),
	.fw = &tu102_fw,
	.booter = &sec2_tu102,
	.loader = GU_FW_SEC2_BL_TU102,
	.fwsec_version = 2,
	.frts = true,
	.feat = &turing_features,
	.memory = GU_MEMORY_RANGE,
	.scrub_via_dmactl = true,
};

#define AD102(id) { .device = id, .chip = 0x192, .family = &ad10x }
#define AD103(id) { .device = id, .chip = 0x193, .family = &ad10x }
#define AD104(id) { .device = id, .chip = 0x194, .family = &ad10x }
#define AD106(id) { .device = id, .chip = 0x196, .family = &ad10x }
#define AD107(id) { .device = id, .chip = 0x197, .family = &ad10x }

#define TU102(id) { .device = id, .chip = 0x162, .family = &tu102 }
#define TU104(id) { .device = id, .chip = 0x164, .family = &tu102 }
#define TU106(id) { .device = id, .chip = 0x166, .family = &tu102 }

#define GA100(id, config, max_mw) \
	{ .device = id, .chip = 0x170, .family = &ga100, .privileged = GU_WRITE_TABLE(config), \
	  .power_max_mw = max_mw }
#define GA102(id) { .device = id, .chip = 0x172, .family = &ga10x }
#define GA103(id) { .device = id, .chip = 0x173, .family = &ga10x }
#define GA104(id) { .device = id, .chip = 0x174, .family = &ga10x }
#define GA106(id) { .device = id, .chip = 0x176, .family = &ga10x }

static const struct gu_card cards[] = {
	/* GA100: CMP 170HX 8GB, CMP 170HX 10GB */
	GA100(0x20c2, cmp170hx_8gb_geometry, 0), GA100(0x2082, cmp170hx_10gb_geometry, 300000),
	
	{ .device = 0x20b5, .chip = 0x170, .family = &ga100_hbm, .power_min_mw = 100000 },

	/* TU102: RTX 2080 Ti, Titan RTX */
	TU102(0x1e03), TU102(0x1e04), TU102(0x1e07), TU102(0x1e2d),
	TU102(0x1e2e),

	/* TU104: RTX 2080 SUPER, RTX 2070 SUPER, RTX 2080, RTX 2070 */
	TU104(0x1e81), TU104(0x1e82), TU104(0x1e84), TU104(0x1e87),
	TU104(0x1e89), TU104(0x1ec2), TU104(0x1ec7),

	/* TU106: RTX 2070, RTX 2060 SUPER, RTX 2060 */
	TU106(0x1f02), TU106(0x1f03), TU106(0x1f06), TU106(0x1f07),
	TU106(0x1f08), TU106(0x1f42), TU106(0x1f47),

	/* AD102: RTX 4090, RTX 4090 D, RTX 4070 Ti SUPER */
	AD102(0x2684), AD102(0x2685), AD102(0x2689),

	/* AD103: RTX 4080 SUPER, RTX 4080, RTX 4070 Ti SUPER, RTX 4070,
	 * RTX 4090 Laptop */
	AD103(0x2702), AD103(0x2704), AD103(0x2705), AD103(0x2709),
	AD103(0x2757),

	/* AD104: RTX 4070 Ti, RTX 4070 SUPER, RTX 4070 */
	AD104(0x2782), AD104(0x2783), AD104(0x2786),

	/* AD106: RTX 4060 Ti, RTX 4060 Ti 16GB */
	AD106(0x2803), AD106(0x2805),

	/* AD107: RTX 4060, RTX 4060 Laptop, RTX 4060 Mobile, RTX 4050 Mobile */
	AD107(0x2882), AD107(0x28a0), AD107(0x28e0), AD107(0x28e1),

	GA102(0x2200), GA102(0x2203), GA102(0x2204), GA102(0x2205),
	GA102(0x2206), GA102(0x2207), GA102(0x2208), GA102(0x220a),
	GA102(0x220d), GA102(0x2216), GA102(0x222b), GA102(0x222f),

	GA103(0x2414),

	GA104(0x2480), GA104(0x2482), GA104(0x2483), GA104(0x2484),
	GA104(0x2486), GA104(0x2487), GA104(0x2488), GA104(0x2489),
	GA104(0x248a), GA104(0x248c), GA104(0x248d), GA104(0x248e),
	GA104(0x249c), GA104(0x249d), GA104(0x249f), GA104(0x24a0),
	GA104(0x24a4), GA104(0x24ac), GA104(0x24ad), GA104(0x24af),
	GA104(0x24bf), GA104(0x24c0), GA104(0x24c7), GA104(0x24c8),
	GA104(0x24c9), GA104(0x24dc), GA104(0x24dd), GA104(0x24df),
	GA104(0x24e0),

	GA106(0x2501), GA106(0x2503), GA106(0x2504), GA106(0x2505),
	GA106(0x2507), GA106(0x2508), GA106(0x2509), GA106(0x252f),
	GA106(0x2544),
};

const struct gu_card *gu_card_find(u16 device)
{
	size_t i;
	for (i = 0; i < sizeof(cards) / sizeof(cards[0]); i++)
		if (cards[i].device == device)
			return &cards[i];
	return NULL;
}

int gu_recipe_prepare(const struct gu_card *card, bool ecc_enabled,
		      struct gu_reg_write **writes, size_t *count)
{
	const struct gu_family *family = card->family;
	const struct gu_ecc_recipe *ecc = family->ecc;
	const struct gu_write_table tables[] = {
		family->selectors,
		family->privileged,
		{ .entries = p2p_privileged,
		  .count = family->p2p ? ARRAY_SIZE(p2p_privileged) : 0 },
		ecc_enabled && ecc ? ecc->prepare : (struct gu_write_table) { },
	};
	struct gu_reg_write *prepared;
	size_t prefix = 0, enable_count, total, used = 0, i;
	size_t slots = family->booter->payload->writes;

	*writes = NULL;
	*count = 0;
	if ((ecc_enabled && !ecc) || slots < 2 || slots > GU_PAYLOAD_MAX_WRITES)
		return -EINVAL;
	/* The executor owns the last slot for WPR2 restoration. */
	slots--;
	for (i = 0; i < ARRAY_SIZE(tables); i++)
		prefix += tables[i].count;
	enable_count = ecc_enabled ? ecc->enabled.count : 0;
	/* Keep dependent ECC mode writes together on a fresh launch. */
	if (enable_count)
		prefix = roundup(prefix, slots);
	total = prefix + enable_count + card->privileged.count;
	/* A geometry suffix must fit entirely in the last invocation. */
	if (card->privileged.count &&
	    (prefix + enable_count) % slots + card->privileged.count > slots)
		return -EINVAL;
	prepared = kcalloc(total, sizeof(*prepared), GFP_KERNEL);
	if (!prepared)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(tables); i++) {
		if (tables[i].count)
			memcpy(prepared + used, tables[i].entries,
			       tables[i].count * sizeof(*prepared));
		used += tables[i].count;
	}
	while (used < prefix) {
		prepared[used] = prepared[used - 1];
		used++;
	}
	if (enable_count) {
		memcpy(prepared + prefix, ecc->enabled.entries,
		       enable_count * sizeof(*prepared));
		/* The common GDDR block starts with the feature override, whose
		 * address moved from FECS to FUSE after Turing. */
		if (family->vram == GU_VRAM_GDDR)
			prepared[prefix].addr = family->feat->ecc;
	}
	/* Geometry changes invalidate the old Booter metadata. They must be
	 * last, including when ECC needs several launches before them. */
	if (card->privileged.count)
		memcpy(prepared + prefix + enable_count, card->privileged.entries,
		       card->privileged.count * sizeof(*prepared));
	*writes = prepared;
	*count = total;
	return 0;
}
