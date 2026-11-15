// SPDX-License-Identifier: GPL-2.0-only
/* Host tests for the parsers and the recipe table, compiled from the same
 * sources the module compiles. Run them against the generated firmware with
 * `make test-unit`, which needs `make assets`.
 */
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

/* The bundle magic of the generated Booter files; BOOTER_MAGIC lives in the
 * kernel-only firmware_610.c. */
#define BOOTER_MAGIC 0x314c5442u

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

	assert(gpu_unlock_gfw_assess(&s) == 0);

	s.boot = 0x10;
	assert(gpu_unlock_gfw_assess(&s) == -EAGAIN);
	s.boot = ~0u;
	assert(gpu_unlock_gfw_assess(&s) == -ENODEV);

	/* An all-ones read is an inaccessible register, not a completed boot. */
	s.boot = GU_GFW_BOOT_COMPLETED;
	s.plm = ~0u;
	assert(gpu_unlock_gfw_assess(&s) == -ENODEV);

	assert(gpu_unlock_pmcsr_d0(0x0000));
	assert(!gpu_unlock_pmcsr_d0(0x0003));
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

	assert(gpu_unlock_wpr_meta_pack(meta, &out, &args) == 0);
	assert(rd32_le(meta) == (u32)(GU_WPR_META_MAGIC & 0xffffffffu));

	assert(gpu_unlock_fuse_version(0) == 0);
	assert(gpu_unlock_fuse_version(1) == 1);
	assert(gpu_unlock_fuse_version(7) == 3);
}

static void test_recipes_and_payloads(void)
{
	/* One representative of every family plus the cards whose owners
	 * reported a measured unlock: GA103M 0x2420, GA106M 0x2520 and 0x2560,
	 * GA107, and the Ada laptop SKUs. */
	static const struct {
		u16 id;
		u32 chip;
	} expected_cards[] = {
		{ 0x1e07, 0x162 }, /* RTX 2080 Ti (TU102) */
		{ 0x1e84, 0x164 }, /* RTX 2070 SUPER (TU104) */
		{ 0x1f08, 0x166 }, /* RTX 2060 (TU106) */
		{ 0x2204, 0x172 }, /* RTX 3090 (GA102) */
		{ 0x2414, 0x173 }, /* RTX 3080 Laptop (GA103) */
		{ 0x2420, 0x173 }, /* RTX 3080 Ti Laptop (GA103) */
		{ 0x2484, 0x174 }, /* RTX 3070 (GA104) */
		{ 0x2504, 0x176 }, /* RTX 3060 12 GB (GA106) */
		{ 0x2520, 0x176 }, /* RTX 3060 Laptop (GA106) */
		{ 0x2560, 0x176 }, /* RTX 3060 Laptop (GA106) */
		{ 0x2582, 0x177 }, /* RTX 3050 (GA107) */
		{ 0x2684, 0x192 }, /* RTX 4090 (AD102) */
		{ 0x2704, 0x193 }, /* RTX 4080 (AD103) */
		{ 0x2786, 0x194 }, /* RTX 4070 (AD104) */
		{ 0x2805, 0x196 }, /* RTX 4060 Ti 16 GB (AD106) */
		{ 0x2820, 0x196 }, /* RTX 4070 Laptop (AD106) */
		{ 0x28a0, 0x197 }, /* RTX 4060 Laptop (AD107) */
	};
	size_t i;

	assert(gu_card_find(0x0000) == NULL);

	for (i = 0; i < sizeof(expected_cards) / sizeof(expected_cards[0]); i++) {
		const struct gu_card *card = gu_card_find(expected_cards[i].id);
		const struct gu_payload *p;
		struct gu_reg_write batch[GU_PAYLOAD_MAX_WRITES];
		size_t n, j;
		u8 *buf;

		assert(card != NULL);
		assert(card->chip == expected_cards[i].chip);
		assert(card->family->fw != NULL);
		assert(card->family->booter != NULL);
		p = card->family->booter->payload;
		assert(p != NULL);
		assert(gu_payload_check(p) == 0);

		n = card->family->privileged.count;
		if (n > p->writes)
			n = p->writes;
		for (j = 0; j < n; j++)
			batch[j] = card->family->privileged.entries[j];

		buf = calloc(1, p->size);
		assert(buf != NULL);
		assert(gu_payload_build(p, buf, batch, n) == 0);
		if (n > 0) {
			assert(rd32_le(buf + p->slots[0].address_off) == batch[0].addr);
			assert(rd32_le(buf + p->slots[0].value_off) == batch[0].value);
		}
		free(buf);
	}
}

static u8 *read_file(const char *dir, const char *file, long *size)
{
	char path[512];
	long sz;
	u8 *data;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	assert(sz > 0);
	data = malloc((size_t)sz);
	assert(data != NULL);
	assert(fread(data, 1, (size_t)sz, f) == (size_t)sz);
	fclose(f);
	*size = sz;
	return data;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

/* Parse the generated assets the way the module parses them at run time,
 * including the Booter bundle geometry of gu_booter_check(). */
static void test_extracted_assets(const char *assets)
{
	static const u16 reps[] = { 0x1e07, 0x2204, 0x2684 };
	size_t i;

	for (i = 0; i < sizeof(reps) / sizeof(reps[0]); i++) {
		const struct gu_card *card = gu_card_find(reps[i]);
		struct gpu_unlock_boot_desc bdesc = { 0 };
		struct gpu_unlock_booter_hdr bhdr = { 0 };
		const struct gu_booter_expect *expect;
		struct gpu_unlock_span span = { 0 };
		long sz;
		u32 image_size, sig_bytes;
		u8 *data;

		assert(card != NULL);
		expect = &card->family->booter->expect;
		data = read_file(assets, base_name(card->family->fw->desc), &sz);
		if (!data) {
			printf("unit_test: no assets in %s, skipped\n", assets);
			return;
		}
		assert(gpu_unlock_boot_desc_parse(data, (size_t)sz,
						  card->family->fw->boot_size,
						  &bdesc) == 0);
		assert(bdesc.image_size == card->family->fw->boot_size);
		free(data);

		data = read_file(assets, base_name(card->family->booter->file), &sz);
		assert(data != NULL);
		assert(rd32_le(data) == BOOTER_MAGIC);
		assert(rd32_le(data + 4) == 1u);
		memcpy(&bhdr, data + 8, sizeof(bhdr));
		memcpy(&image_size, data + 8 + sizeof(bhdr), sizeof(image_size));
		memcpy(&sig_bytes, data + 12 + sizeof(bhdr), sizeof(sig_bytes));
		assert(image_size == expect->image_size);
		assert(sig_bytes == expect->sig_bytes);
		assert((long)sz == 72 + (long)image_size + (long)sig_bytes);
		assert(bhdr.ucode_id == expect->ucode_id);
		assert(bhdr.engine_id == 1 && bhdr.num_apps == 1);
		assert(bhdr.num_sigs == expect->num_sigs);
		assert(bhdr.os_code_off == 0 && bhdr.os_code_size == 256);
		assert(bhdr.os_data_off == expect->os_data_off);
		assert(bhdr.os_data_size == expect->os_data_size);
		assert(bhdr.app_code_off == 256 && bhdr.app_data_off == 256);
		assert(bhdr.app_data_size == 0);
		assert(bhdr.patch_loc == expect->patch_loc);
		assert(gpu_unlock_booter_patch(data + 72, image_size, &bhdr,
					       data + 72 + image_size,
					       sig_bytes, 0) == 0);
		free(data);

		data = read_file(assets, base_name(card->family->fw->gsp), &sz);
		assert(data != NULL);
		assert(gpu_unlock_elf_section(data, (size_t)sz, ".fwimage", &span) == 0);
		assert(span.size > 0);
		assert(gpu_unlock_elf_section(data, (size_t)sz,
					      card->family->fw->signature_section,
					      &span) == 0);
		assert(span.size == 4096);
		free(data);
	}
	printf("unit_test: boot descriptor, Booter and GSP ELF of %s verified\n", assets);
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
