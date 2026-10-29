// SPDX-License-Identifier: GPL-2.0-only
#include "recipes.h"
#include "registers.h"

/* GA102 re-entrant call table. The Booter dispatches the frame RET into
 * these gadgets and performs the privileged writes from the value/address
 * slots. Offsets are relative to the signature image, not to BAR0.
 *
 * The write entry point and the six-write frame layout are Petri Krohn's:
 * enter reg_write_indirect() at 0x1110 with the address in r0 and the value in
 * r1, so the 0x1fce parameter shuffle is unnecessary. Skipping the prologue
 * also means r3 must already point at a word equal to r9 for the 0x1149 canary
 * check (0x6140 works: it holds the same canary); otherwise 0x811b stores 0x47
 * to I[0x1000] and spins. The skipped prologue also moves the epilogue frame:
 * 0x1156 pops r3..r0 from [sp..sp+0xc], skips the canary slot at sp+0x10 and
 * returns via sp+0x14, so one write consumes 0x18 bytes and the popped r0/r1
 * carry the next write's parameters.
 *
 * Stack: writes start at 0xff54, one 0x18 frame each. Cleanup starts at
 * 0xff54 + 6*0x18 = 0xffe4: 0x576b (SE/CSB handshake, data pointer derived
 * from sp) leaves r0=3, 0x84c8 turns that into r10=3 while r11's low byte is
 * already 0, then 0xd52 and the 0x81ee exit. The final return word is at
 * 0xfff4; a seventh write would need 0x1000c. The driver batches the family's
 * write table into launches of six and repeats the last entry to fill. */
static const struct gu_payload_word ga102_words[] = {
	{ 0x1100, 0x00000007 },
	{ 0xf93c, 0x00006140 },	/* 0x686 pop: r3 canary pointer, write 1 */
	{ 0xf950, 0x00001110 },	/* write 1: r0=address, r1=value */
	{ 0xf954, 0x00006140 },	/* frame 1: r3 for write 2 */
	{ 0xf968, 0x00001110 },	/* frame 1: ret -> write 2 */
	{ 0xf96c, 0x00006140 },	/* frame 2: r3 for write 3 */
	{ 0xf980, 0x00001110 },	/* frame 2: ret -> write 3 */
	{ 0xf984, 0x00006140 },	/* frame 3: r3 for write 4 */
	{ 0xf998, 0x00001110 },	/* frame 3: ret -> write 4 */
	{ 0xf99c, 0x00006140 },	/* frame 4: r3 for write 5 */
	{ 0xf9b0, 0x00001110 },	/* frame 4: ret -> write 5 */
	{ 0xf9b4, 0x00006140 },	/* frame 5: r3 for write 6 */
	{ 0xf9c8, 0x00001110 },	/* frame 5: ret -> write 6 */
	{ 0xf9cc, 0x00006140 },	/* frame 6: r3 (unused by cleanup) */
	{ 0xf9d8, 0x00000003 },	/* frame 6: r0 = 3 for 0xd52 */
	{ 0xf9e0, 0x0000576b },	/* frame 6: ret -> SE/CSB handshake */
	{ 0xf9e4, 0x000084c8 },	/* cleanup: r10 = r0 = 3, ret 0xd52 */
	{ 0xf9f0, 0x00000d52 },	/* cleanup: final pass */
	{ 0xf9f4, 0x000081ee },	/* HS clean exit */
};
static const struct gu_payload_slot ga102_slots[] = {
	{ 0xf944, 0xf948 },
	{ 0xf95c, 0xf960 },
	{ 0xf974, 0xf978 },
	{ 0xf98c, 0xf990 },
	{ 0xf9a4, 0xf9a8 },
	{ 0xf9bc, 0xf9c0 },
};
#define GU_PAYLOAD_V67(label) {					\
	.name = label,						\
	.size = 0xfa00,						\
	.fill = 0x0000019c,					\
	.slots = ga102_slots,				\
	.writes = sizeof(ga102_slots) / sizeof(ga102_slots[0]), \
	.words = ga102_words,				\
	.word_count = sizeof(ga102_words) / sizeof(ga102_words[0]), \
	.completion = GU_PAYLOAD_HALT_RESET_PLM,		\
}

static const struct gu_payload ga102_v67 = GU_PAYLOAD_V67("ga102-v67");
/* No payload has been derived for the GA100 signed Booter yet; it reuses the
 * GA102 V67 body until one is. Replace the descriptor when it lands. */
static const struct gu_payload ga100_v67 = GU_PAYLOAD_V67("ga100-v67");

/* AD102 port of Petri Krohn's GA102 six-write chain. The two signed Booters
 * are the same code with functions moved, so the GA102 gadgets map one-for-one
 * through the fwmap: the signature epilogue 0x686 -> 0x1e02, the write entry
 * 0x1110 -> 0x257e, the 0x84c8 gadget -> 0x7a02 and the HS exit
 * 0x81ee -> 0x771e. The canary global moves 0x6140 -> 0x5f50, while the DMEM
 * frame layout is unchanged: the signature epilogue still starts at 0xff30,
 * so the six 0x18-byte write frames and their slots use the same offsets.
 *
 * The GA102 cleanup starts with the 0x576b status read at CSB address 0x114;
 * AD102 has no exact counterpart for that function (the fwmap's fuzzy 0x73ec
 * match performs a different table read), so the port keeps the AD102
 * booter's own proven marker exit from the TOTORONG/AD103 chain:
 * 0x7a02 moves the marker 0x67b00001 into r10, 0x31b5 publishes it to
 * I[0x1000], and 0x771e exits. That matches this payload's MARKER completion
 * and leaves the final return at 0xfff0. */
static const struct gu_payload_slot ad102_v67_slots[] = {
	{ 0xf944, 0xf948 },
	{ 0xf95c, 0xf960 },
	{ 0xf974, 0xf978 },
	{ 0xf98c, 0xf990 },
	{ 0xf9a4, 0xf9a8 },
	{ 0xf9bc, 0xf9c0 },
};
static const struct gu_payload_word ad102_v67_words[] = {
	{ 0x1100, 0x00000007 },
	{ 0xf93c, 0x00005f50 },	/* 0x1e02 pop: r3 canary pointer, write 1 */
	{ 0xf950, 0x0000257e },	/* write 1: r0=address, r1=value */
	{ 0xf954, 0x00005f50 },	/* frame 1: r3 for write 2 */
	{ 0xf968, 0x0000257e },	/* frame 1: ret -> write 2 */
	{ 0xf96c, 0x00005f50 },	/* frame 2: r3 for write 3 */
	{ 0xf980, 0x0000257e },	/* frame 2: ret -> write 3 */
	{ 0xf984, 0x00005f50 },	/* frame 3: r3 for write 4 */
	{ 0xf998, 0x0000257e },	/* frame 3: ret -> write 4 */
	{ 0xf99c, 0x00005f50 },	/* frame 4: r3 for write 5 */
	{ 0xf9b0, 0x0000257e },	/* frame 4: ret -> write 5 */
	{ 0xf9b4, 0x00005f50 },	/* frame 5: r3 for write 6 */
	{ 0xf9c8, 0x0000257e },	/* frame 5: ret -> write 6 */
	{ 0xf9cc, 0x00005f50 },	/* frame 6: r3 (unused by cleanup) */
	{ 0xf9d8, 0x67b00001 },	/* frame 6: r0 = completion marker */
	{ 0xf9e0, 0x00007a02 },	/* frame 6: ret -> marker gadget */
	{ 0xf9ec, 0x000031b5 },	/* marker gadget: publish I[0x1000] */
	{ 0xf9f0, 0x0000771e },	/* HS clean exit */
};
static const struct gu_payload ad102_v67 = {
	.name = "ad102-v67",
	.size = 0xfa00,
	.fill = 0x0000019c,
	.slots = ad102_v67_slots,
	.writes = sizeof(ad102_v67_slots) / sizeof(ad102_v67_slots[0]),
	.words = ad102_v67_words,
	.word_count = sizeof(ad102_v67_words) / sizeof(ad102_v67_words[0]),
	.completion = GU_PAYLOAD_HALT_RESET_PLM_MARKER,
	.marker = 0x67b00001,
};

/* TU102 port of the same six-write chain from Petri Krohn. The two signed Booters 
 * are that same code with functions moved, and the fwmap resolves the gadgets exactly
 * (score 1.0): the write entry 0x1110 -> 0x10b9, the status read
 * 0x576b -> 0x6521, the cleanup gadget 0x84c8 -> 0x7f27 and the final pass
 * 0xd52 -> 0xccb. The 0x18-byte frames and the 0x6140 canary are identical,
 * so the six frames sit at the same relative offsets from 0xf940.
 *
 * The frame 6 handoff stays at 0x7ea4, the Turing normal main handoff, rather
 * than the fwmap twin of GA102's 0x81ee (0x7cfc): the latter halts inside the
 * exit path and leaves the Booter's boot-stage marker (0x31) in MAILBOX0,
 * while 0x7ea4 runs the subsystem completion that publishes the clean zero. */
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
	{ 0xf938, 0x00006140 },	/* 0x10b9 pop: r3 canary pointer, write 1 */
	{ 0xf94c, 0x000010b9 },	/* write 1: r0=address, r1=value */
	{ 0xf950, 0x00006140 },	/* frame 1: r3 for write 2 */
	{ 0xf964, 0x000010b9 },	/* frame 1: ret -> write 2 */
	{ 0xf968, 0x00006140 },	/* frame 2: r3 for write 3 */
	{ 0xf97c, 0x000010b9 },	/* frame 2: ret -> write 3 */
	{ 0xf980, 0x00006140 },	/* frame 3: r3 for write 4 */
	{ 0xf994, 0x000010b9 },	/* frame 3: ret -> write 4 */
	{ 0xf998, 0x00006140 },	/* frame 4: r3 for write 5 */
	{ 0xf9ac, 0x000010b9 },	/* frame 4: ret -> write 5 */
	{ 0xf9b0, 0x00006140 },	/* frame 5: r3 for write 6 */
	{ 0xf9c4, 0x000010b9 },	/* frame 5: ret -> write 6 */
	{ 0xf9c8, 0x00006140 },	/* frame 6: r3 (unused by cleanup) */
	{ 0xf9d4, 0x00000003 },	/* frame 6: r0 = 3 for 0xccb */
	{ 0xf9dc, 0x00006521 },	/* frame 6: ret -> SE/CSB handshake */
	{ 0xf9e0, 0x00007f27 },	/* cleanup gadget */
	{ 0xf9ec, 0x00000ccb },	/* cleanup: final pass */
	{ 0xf9f0, 0x00007ea4 },	/* HS clean exit */
};
static const struct gu_payload tu102_v67 = {
	.name = "tu102-v67",
	.size = 0xfa00,
	.fill = 0x000004a7,
	.slots = tu102_slots,
	.writes = sizeof(tu102_slots) / sizeof(tu102_slots[0]),
	.words = tu102_words,
	.word_count = sizeof(tu102_words) / sizeof(tu102_words[0]),
	.completion = GU_PAYLOAD_HALT_RESET_PLM_ZERO,
};

/* Turing's FEAT aperture is at 0x409650. Opening WPR_CFG_PLM first lets the
 * driver's own WPR2 restore through the PLM gate; the second write opens the
 * FEAT aperture the SS0/SS1 selectors live behind. No WPR2 poke is needed:
 * restore_frts() writes it with the gate open. */
static const struct gu_reg_write tu102_plm[] = {
	{ GU_WPR_CFG_PLM_OFFSET,    0xfffff0ff },
	{ GU_TU102_FEAT_PLM_OFFSET, 0xffffffff },
};
static const struct gu_reg_write tu102_selectors[] = {
	{ GU_TU102_FEAT_SS0_OFFSET, GU_SS0_UNLOCK },
	{ GU_TU102_FEAT_SS1_OFFSET, GU_SS1_UNLOCK },
};

/* Same per-entry image as GA10x. These are the stores, in order. The FBPA
 * store is attempted by the signed chain but hardware leaves the field at its
 * pre-existing value, so it is optional for the readback gate. */
static const struct gu_reg_write ad102_plm[] = {
	{ GU_WPR_CFG_PLM_OFFSET,	0xfffff0ff },
	{ GU_FBPA_PLM_OFFSET,		0xffffffff, 0xffffff0f, true },
	{ GU_WPR_PLM_OFFSET, 		0xffffffff },
	{ GU_FEAT_PLM_OFFSET, 		0xffffffff },
};

/* Only the masks needed by the GA10x compute recipe. */
static const struct gu_reg_write ga10x_plm[] = {
	{ GU_WPR_CFG_PLM_OFFSET, 	0xfffff0ff },
	{ GU_FEAT_PLM_OFFSET,    	0xffffffff },
};

/* cmpunlocker 2d10f891, sec2-postbl-plm-ss-cfg.patch, plmTable.
 * Order and WPR_CFG's source-control bits are significant. No geometry,
 * link retraining, ECC or GPC/TPC configuration is performed in Stage 1.
 */
static const struct gu_reg_write ga100_plm[] = {
	{ GU_WPR_CFG_PLM_OFFSET,   	0xfffff0ff },
	{ GU_FBPA_PLM_OFFSET,      	0xffffffff },
	{ GU_WPR_PLM_OFFSET,       	0xffffffff },
	{ GU_FEAT_PLM_OFFSET,      	0xffffffff },
	{ GU_XVE_PLM_OFFSET,       	0xffffffff },
	{ GU_XVE_B_PLM_OFFSET,     	0xffffffff },
	{ GU_XVE_C_PLM_OFFSET,     	0xffffffff },
	{ GU_FEAT2_PLM_OFFSET,     	0xffffffff },
	{ GU_OPT_PLM_OFFSET,       	0xffffffff },
	{ GU_RECONFIG_PLM_OFFSET,  	0xffffffff },
	{ GU_PJTAG_PLM_OFFSET,     	0xffffffff },
	{ GU_PJTAG_SEC_PLM_OFFSET, 	0xffffffff },
};

/* Host side writes for speed unlocks */
static const struct gu_reg_write selectors[] = {
	{ GU_FEAT_SS0_OFFSET, GU_SS0_UNLOCK }, 
	{ GU_FEAT_SS1_OFFSET, GU_SS1_UNLOCK },
};

static const struct gu_booter sec2_ga100 = {
	.file = GU_FW_BOOTER_GA100,
	.launch = GU_SEC2_DIRECT,
	.payload = &ga100_v67,
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
	.payload = &ga102_v67,
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
	.payload = &ad102_v67,
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
	.payload = &tu102_v67,
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

static const struct gu_family ga100 = {
	.name = "GA100",
	.privileged = GU_WRITE_TABLE(ga100_plm),
	.host = GU_WRITE_TABLE(selectors),
	.fw = &ga100_fw,
	.booter = &sec2_ga100,
	.loader = GU_FW_LOADER,
	.fwsec_version = 2,
	.feat_plm = GU_FEAT_PLM_OFFSET,
	.feat_ss0 = GU_FEAT_SS0_OFFSET,
	.feat_ss1 = GU_FEAT_SS1_OFFSET,
};
static const struct gu_family ga10x = {
	.name = "GA102/GA103/GA104/GA106",
	.privileged = GU_WRITE_TABLE(ga10x_plm),
	.host = GU_WRITE_TABLE(selectors),
	.fw = &ga10x_fw,
	.booter = &sec2_ga102,
	.loader = NULL,
	.fwsec_version = 3,
	.feat_plm = GU_FEAT_PLM_OFFSET,
	.feat_ss0 = GU_FEAT_SS0_OFFSET,
	.feat_ss1 = GU_FEAT_SS1_OFFSET,
	.direct_selectors = true,
};
static const struct gu_family ad10x = {
	.name = "AD102/AD103/AD104/AD106/AD107",
	.privileged = GU_WRITE_TABLE(ad102_plm),
	.host = GU_WRITE_TABLE(selectors),
	.fw = &ad102_fw,
	.booter = &sec2_ad102,
	.loader = NULL,
	.fwsec_version = 3,
	.feat_plm = GU_FEAT_PLM_OFFSET,
	.feat_ss0 = GU_FEAT_SS0_OFFSET,
	.feat_ss1 = GU_FEAT_SS1_OFFSET,
};
static const struct gu_family tu102 = {
	.name = "TU102/TU104/TU106",
	.privileged = GU_WRITE_TABLE(tu102_plm),
	.host = GU_WRITE_TABLE(tu102_selectors),
	.fw = &tu102_fw,
	.booter = &sec2_tu102,
	.loader = GU_FW_SEC2_BL_TU102,
	.fwsec_version = 2,
	.feat_plm = GU_TU102_FEAT_PLM_OFFSET,
	.feat_ss0 = GU_TU102_FEAT_SS0_OFFSET,
	.feat_ss1 = GU_TU102_FEAT_SS1_OFFSET,
	.turing_layout = true,
};

#define AD102(id) { id, 0x192, &ad10x }
#define AD103(id) { id, 0x193, &ad10x }
#define AD104(id) { id, 0x194, &ad10x }
#define AD106(id) { id, 0x196, &ad10x }
#define AD107(id) { id, 0x197, &ad10x }

#define TU102(id) { id, 0x162, &tu102 }
#define TU104(id) { id, 0x164, &tu102 }
#define TU106(id) { id, 0x166, &tu102 }

#define GA100(id) { id, 0x170, &ga100 }
#define GA102(id) { id, 0x172, &ga10x }
#define GA103(id) { id, 0x173, &ga10x }
#define GA104(id) { id, 0x174, &ga10x }
#define GA106(id) { id, 0x176, &ga10x }

static const struct gu_card cards[] = {
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
