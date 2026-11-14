// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/gpu_unlock/gfw_eval.h"
#include "../kernel/gpu_unlock/layout_610.h"
#include "../kernel/gpu_unlock/payload.h"
#include "../kernel/gpu_unlock/prep_610.h"
#include "../kernel/gpu_unlock/recipes.h"
#include "../kernel/gpu_unlock/registers.h"
#include "../kernel/gpu_unlock/vbios_610.h"

static u32 rd32_le(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void test_gfw_eval(void)
{
	struct gpu_unlock_gfw_sample s = {
		.config_ok = true,
		.plm = GU_GFW_PLM_LEVEL0,
		.boot = GU_GFW_BOOT_COMPLETED,
		.cpuctl = GU_GFW_CPUCTL_HALTED,
	};

	assert(gpu_unlock_gfw_assess(NULL) == -ENODEV);
	assert(gpu_unlock_gfw_assess(&s) == 0);

	s.boot = 0x10;
	assert(gpu_unlock_gfw_assess(&s) == -EAGAIN);
	s.boot = ~0u;
	assert(gpu_unlock_gfw_assess(&s) == -ENODEV);

	assert(gpu_unlock_pmcsr_d0(0x0000) == true);
	assert(gpu_unlock_pmcsr_d0(0x0003) == false);
	assert(gpu_unlock_command_restore(0x0006, 0x0100) == 0x0106);
}

static void test_layout_and_prep(void)
{
	struct gpu_unlock_layout_in in = {
		.usable_fb = 8ull << 30,
		.vga_base = (8ull << 30) - (1ull << 20),
		.mmu_lock_valid = false,
		.frts_size = 1ull << 20,
		.boot_size = 36864,
		.image_size = 84226048,
		.nonwpr_size = 1ull << 20,
		.heap_request = 88ull << 20,
	};
	struct gpu_unlock_layout_out out = { 0 };
	struct gpu_unlock_radix_plan plan = { 0 };
	u8 meta[GU_WPR_META_SIZE];

	assert(gpu_unlock_layout(&in, &out) == 0);
	assert(out.nonwpr_start < out.wpr_start);
	assert(out.wpr_start < out.heap_offset);
	assert(out.heap_offset < out.gsp_offset);
	assert(out.gsp_offset < out.boot_offset);
	assert(out.boot_offset < out.frts_offset);
	assert(out.frts_offset < out.wpr_end);

	assert(gpu_unlock_radix_plan(in.image_size, &plan) == 0);
	assert(plan.pages[0] == 1);
	assert(plan.table_pages > 0);

	struct gpu_unlock_wpr_args args = {
		.fb_size = in.usable_fb,
		.vga_offset = in.vga_base,
		.frts_size = in.frts_size,
		.signature_dma = 0x100000,
		.signature_size = 0xfa00,
		.radix_dma = 0x200000,
		.image_size = in.image_size,
		.boot_dma = 0x300000,
		.boot = {
			.code_offset = 0x100,
			.data_offset = 0x2000,
			.manifest_offset = 0x3000,
			.image_size = 36864,
		},
	};
	assert(gpu_unlock_wpr_meta_pack(meta, &out, &args) == 0);
	assert(rd32_le(meta) == (u32)(GU_WPR_META_MAGIC & 0xffffffffu));

	assert(gpu_unlock_fuse_version(0) == 0);
	assert(gpu_unlock_fuse_version(1) == 1);
	assert(gpu_unlock_fuse_version(7) == 3);
}

static void test_recipes_and_payloads(void)
{
	/* Verify key desktop and laptop cards across all supported families,
	 * including RTX 4070 Laptop (0x2820) and regressed GA106M (0x2520, 0x2560). */
	static const struct {
		u16 id;
		u32 chip;
	} expected_cards[] = {
		{ 0x1e02, 0x162 }, /* Titan RTX (TU102) */
		{ 0x1e07, 0x162 }, /* RTX 2080 Ti (TU102) */
		{ 0x2204, 0x172 }, /* RTX 3090 (GA102) */
		{ 0x2420, 0x173 }, /* RTX 3080 Ti Laptop (GA103) */
		{ 0x249d, 0x174 }, /* RTX 3070 Laptop (GA104) */
		{ 0x2504, 0x176 }, /* RTX 3060 (GA106) */
		{ 0x2520, 0x176 }, /* RTX 3060 Laptop (GA106) */
		{ 0x2560, 0x176 }, /* RTX 3060 Laptop (GA106) */
		{ 0x2684, 0x192 }, /* RTX 4090 (AD102) */
		{ 0x2704, 0x193 }, /* RTX 4080 (AD103) */
		{ 0x2717, 0x193 }, /* RTX 4090 Laptop (AD103) */
		{ 0x2757, 0x193 }, /* RTX 4090 Laptop (AD103) */
		{ 0x2786, 0x194 }, /* RTX 4070 (AD104) */
		{ 0x27a0, 0x194 }, /* RTX 4080 Laptop (AD104) */
		{ 0x2803, 0x196 }, /* RTX 4060 Ti (AD106) */
		{ 0x2820, 0x196 }, /* RTX 4070 Laptop (AD106) */
		{ 0x2860, 0x196 }, /* RTX 4070 Laptop (AD106) */
		{ 0x28a0, 0x197 }, /* RTX 4060 Laptop (AD107) */
		{ 0x28a1, 0x197 }, /* RTX 4050 Laptop (AD107) */
		{ 0x28e0, 0x197 }, /* RTX 4060 Laptop (AD107) */
	};
	size_t i;

	assert(gu_card_find(0x0000) == NULL);

	for (i = 0; i < sizeof(expected_cards) / sizeof(expected_cards[0]); i++) {
		const struct gu_card *card = gu_card_find(expected_cards[i].id);
		assert(card != NULL);
		assert(card->chip == expected_cards[i].chip);
		assert(card->family != NULL);
		assert(card->family->fw != NULL);
		assert(card->family->booter != NULL);
		assert(card->family->booter->payload != NULL);
		assert(gu_payload_check(card->family->booter->payload) == 0);

		const struct gu_payload *p = card->family->booter->payload;
		u8 *buf = calloc(1, p->size);
		assert(buf != NULL);
		size_t n = card->family->privileged.count;
		if (n > p->writes)
			n = p->writes;
		assert(gu_payload_build(p, buf, card->family->privileged.entries, n) == 0);
		if (n > 0) {
			assert(rd32_le(buf + p->slots[0].value_off) ==
			       card->family->privileged.entries[0].value);
			assert(rd32_le(buf + p->slots[0].address_off) ==
			       card->family->privileged.entries[0].addr);
		}
		free(buf);
	}
}

static void test_extracted_assets(const char *assets_dir)
{
	static const u16 family_reps[] = { 0x1e07, 0x2204, 0x2820 };
	size_t i;

	for (i = 0; i < sizeof(family_reps) / sizeof(family_reps[0]); i++) {
		const struct gu_card *card = gu_card_find(family_reps[i]);
		char path[512];
		const char *base;
		FILE *f;
		long sz;
		u8 *data;
		struct gpu_unlock_boot_desc bdesc = { 0 };
		struct gpu_unlock_booter_hdr bhdr = { 0 };
		struct gpu_unlock_span span = { 0 };

		assert(card != NULL);
		base = strrchr(card->family->fw->desc, '/');
		base = base ? base + 1 : card->family->fw->desc;
		snprintf(path, sizeof(path), "%s/%s", assets_dir, base);
		f = fopen(path, "rb");
		if (!f) {
			printf("skip live asset check (not found: %s)\n", path);
			return;
		}
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		assert(sz > 0);
		data = malloc((size_t)sz);
		assert(data != NULL);
		assert(fread(data, 1, (size_t)sz, f) == (size_t)sz);
		fclose(f);

		assert(gpu_unlock_boot_desc_parse(data, (size_t)sz,
						  card->family->fw->boot_size,
						  &bdesc) == 0);
		assert(bdesc.image_size == card->family->fw->boot_size);
		free(data);

		/* Verify Booter bundle header and signature patching */
		base = strrchr(card->family->booter->file, '/');
		base = base ? base + 1 : card->family->booter->file;
		snprintf(path, sizeof(path), "%s/%s", assets_dir, base);
		f = fopen(path, "rb");
		assert(f != NULL);
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		assert(sz > 72);
		data = malloc((size_t)sz);
		assert(data != NULL);
		assert(fread(data, 1, (size_t)sz, f) == (size_t)sz);
		fclose(f);

		assert(rd32_le(data) == 0x314c5442u);
		assert(rd32_le(data + 4) == 1u);
		memcpy(&bhdr, data + 8, sizeof(bhdr));
		assert(bhdr.ucode_id == card->family->booter->expect.ucode_id);
		assert(bhdr.patch_loc == card->family->booter->expect.patch_loc);
		assert(bhdr.sig_bytes == card->family->booter->expect.sig_bytes);
		u32 img_bytes = bhdr.os_data_off + bhdr.os_data_size;
		assert(img_bytes == card->family->booter->expect.image_size);
		assert(gpu_unlock_booter_patch(data + 72, img_bytes, &bhdr,
					       data + 72 + img_bytes, bhdr.sig_bytes, 0) == 0);
		free(data);

		/* Verify GSP ELF sections (.fwimage and family signature section) */
		base = strrchr(card->family->fw->gsp, '/');
		base = base ? base + 1 : card->family->fw->gsp;
		snprintf(path, sizeof(path), "%s/%s", assets_dir, base);
		f = fopen(path, "rb");
		assert(f != NULL);
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		assert(sz > 64);
		data = malloc((size_t)sz);
		assert(data != NULL);
		assert(fread(data, 1, (size_t)sz, f) == (size_t)sz);
		fclose(f);

		assert(gpu_unlock_elf_section(data, (size_t)sz, ".fwimage", &span) == 0);
		assert(span.size > 0);
		assert(gpu_unlock_elf_section(data, (size_t)sz,
					      card->family->fw->signature_section, &span) == 0);
		assert(span.size == 4096);
		free(data);
	}
	printf("live extracted assets (desc, booter, GSP ELF) verified from %s\n", assets_dir);
}

int main(int argc, char **argv)
{
	test_gfw_eval();
	test_layout_and_prep();
	test_recipes_and_payloads();
	if (argc > 1)
		test_extracted_assets(argv[1]);
	printf("unit_test: all checks passed\n");
	return 0;
}
