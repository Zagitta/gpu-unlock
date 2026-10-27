// SPDX-License-Identifier: GPL-2.0-only
#include "recipes.h"
#include "registers.h"

/* GA102 re-entrant V67 call table. The Booter dispatches the frame RET into
 * these gadgets and performs the privileged write from the value/address
 * slots, so one invocation applies one table entry. Offsets are relative to
 * the signature image, not to BAR0. */
static const struct gu_payload_word ga102_v67_words[] = {
	{ 0x1100, 0x00000007 },
	{ 0xf950, 0x00000d44 },
	{ 0xf968, 0x00001fce },
	{ 0xf974, 0x00000000 },
	{ 0xf97c, 0x00001101 },
	{ 0xf980, 0x000084c8 },
	{ 0xf984, 0x00008e18 },
	{ 0xf98c, 0x000084c8 },
	{ 0xf990, 0x00000000 },
	{ 0xf998, 0x00001fce },
	{ 0xf9a4, 0x0000ffb0 },
	{ 0xf9ac, 0x00005789 },
	{ 0xf9bc, 0x00000d44 },
	{ 0xf9cc, 0x00000003 },
	{ 0xf9d4, 0x00001fce },
	{ 0xf9e8, 0x00000d52 },
	{ 0xf9ec, 0x000081ee },
};
#define GU_PAYLOAD_V67(label) {					\
	.name = label,						\
	.size = 0xfa00,						\
	.fill = 0x0000019c,					\
	.value_off = 0xf948,					\
	.address_off = 0xf960,					\
	.words = ga102_v67_words,				\
	.word_count = sizeof(ga102_v67_words) / sizeof(ga102_v67_words[0]), \
	.completion = GU_PAYLOAD_HALT_RESET_PLM,		\
}

static const struct gu_payload ga102_v67 = GU_PAYLOAD_V67("ga102-v67");
/* No payload has been derived for the GA100 or AD102 signed Booter yet; both
 * reuse the GA102 V67 body until one is. Replace the descriptor when it lands. */
static const struct gu_payload ga100_v67 = GU_PAYLOAD_V67("ga100-v67");
static const struct gu_payload ad102_v67 = GU_PAYLOAD_V67("ad102-v67");

/* Same per-entry image as GA10x. These are the stores, in order. */
static const struct gu_reg_write ad102_plm[] = {
	{ GU_WPR_CFG_PLM_OFFSET,	0xfffff0ff },
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

static const struct gu_family ga100 = {
	.name = "GA100",
	.privileged = GU_WRITE_TABLE(ga100_plm),
	.host = GU_WRITE_TABLE(selectors),
	.fw = &ga100_fw,
	.booter = &sec2_ga100,
	.loader = GU_FW_LOADER,
	.fwsec_version = 2,
};
static const struct gu_family ga10x = {
	.name = "GA102/GA103/GA104/GA106",
	.privileged = GU_WRITE_TABLE(ga10x_plm),
	.host = GU_WRITE_TABLE(selectors),
	.fw = &ga10x_fw,
	.booter = &sec2_ga102,
	.loader = NULL,
	.fwsec_version = 3,
};
static const struct gu_family ad102 = {
	.name = "AD102",
	.privileged = GU_WRITE_TABLE(ad102_plm),
	.host = GU_WRITE_TABLE(selectors),
	.fw = &ad102_fw,
	.booter = &sec2_ad102,
	.loader = NULL,
	.fwsec_version = 3,
};

#define AD102(id) { id, 0x192, &ad102 }

#define GA100(id) { id, 0x170, &ga100 }
#define GA102(id) { id, 0x172, &ga10x }
#define GA103(id) { id, 0x173, &ga10x }
#define GA104(id) { id, 0x174, &ga10x }
#define GA106(id) { id, 0x176, &ga10x }
#define GA107(id) { id, 0x177, &ga10x }

static const struct gu_card cards[] = {
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

	GA107(0x25a0), GA107(0x25a2), GA107(0x25a4), GA107(0x25a5),
	GA107(0x25a6), GA107(0x25a7), GA107(0x25a9), GA107(0x25aa),
	GA107(0x25ab), GA107(0x25ac), GA107(0x25ad), GA107(0x25af),
	GA107(0x25b6), GA107(0x25b8), GA107(0x25b9), GA107(0x25ba),
	GA107(0x25bb), GA107(0x25bc), GA107(0x25bd), GA107(0x25be),
	GA107(0x25bf), GA107(0x25e0), GA107(0x25e2), GA107(0x25e5),
	GA107(0x25f9), GA107(0x25fa), GA107(0x25fb),
};

const struct gu_card *gu_card_find(u16 device)
{
	size_t i;
	for (i = 0; i < sizeof(cards) / sizeof(cards[0]); i++)
		if (cards[i].device == device)
			return &cards[i];
	return NULL;
}
