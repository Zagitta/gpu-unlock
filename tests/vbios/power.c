// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "vbios.h"

/* Small PCI/BIT/NPDS fixtures use the reviewed GA100 formats. Vary container
 * framing, EFI placement, BIT stride, table offsets and the selected TGP row. */
struct fixture {
	u8 rom[0x6000];
	u32 base, adjust, power, entry, perf, clocks, programs, end, bit, token;
	u8 version[5];
};

static u32 read32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void write16(u8 *p, u16 value)
{
	p[0] = value;
	p[1] = value >> 8;
}

static void write32(u8 *p, u32 value)
{
	write16(p, value);
	write16(p + 2, value >> 16);
}

static unsigned int sum(const u8 *p, size_t length)
{
	unsigned int total = 0;
	for (size_t i = 0; i < length; i++)
		total += p[i];
	return total & 255;
}

static void expansion(u8 *image, u32 bytes, u8 type, bool last)
{
	write16(image, 0xaa55);
	write16(image + 0x18, 0x20);
	memcpy(image + 0x20, type == 0xe0 ? "NPDS" : "PCIR", 4);
	write16(image + 0x24, 0x10de);
	write16(image + 0x2a, 0x18);
	write16(image + 0x30, bytes / 512);
	image[0x34] = type;
	image[0x35] = last ? 0x80 : 0;
}

static void checksum(struct fixture *f)
{
	u8 *rom = f->rom + f->base;
	u32 start = 0x400 + f->adjust;
	rom[f->end - 1] = 0;
	rom[f->end - 1] = -sum(rom + start, f->end - start);
}

static void fixture(struct fixture *f, bool container, bool efi, u8 stride,
		    u32 table, u8 tgp, u32 minimum, u32 rated, u32 maximum)
{
	static const u16 floors[] = { 210, 615, 1215, 630, 27, 585, 540, 27, 1, 555 };
	u8 *rom, *bit, *token, *p0;

	memset(f, 0, sizeof(*f));
	f->base = container ? 0x2000 : 0;
	f->adjust = efi ? 0x200 : 0;
	f->power = table + f->adjust;
	f->entry = f->power + 0x2c + tgp * 0x43;
	f->perf = 0x800 + f->adjust;
	f->clocks = 0x1a00 + f->adjust;
	f->programs = 0x1b00 + f->adjust;
	f->end = 0x2400 + f->adjust;
	f->bit = 0x50;
	f->token = stride == 8 ? 0x1c0 : 0x140;
	memcpy(f->version, container ? "\x92\x00\xa1\x01\x05" : "\x92\x00\x66\x00\x02", 5);
	if (container) {
		memcpy(f->rom, "NVGI", 4);
		write32(f->rom + 4, 0x300);
		write32(f->rom + 8, 0x20);
		write32(f->rom + 0x20, 0x100);
		memcpy(f->rom + 0x1100, "RFRD", 4);
		write32(f->rom + 0x1108, f->base);
	}
	rom = f->rom + f->base;
	expansion(rom, 0x400, 0, false);
	if (efi)
		expansion(rom + 0x400, 0x200, 3, false);
	expansion(rom + 0x400 + f->adjust, 0x2000, 0xe0, true);
	bit = rom + f->bit;
	memcpy(bit, "\xff\xb8" "BIT\0\x00\x01\x0c", 9);
	bit[9] = stride;
	bit[10] = 3;
	bit[11] = -sum(bit, 12);
	token = bit + 12;
	token[0] = 'B';
	token[1] = 2;
	write16(token + 2, 5);
	write16(token + 4, 0x100);
	rom[0x100] = f->version[3];
	rom[0x101] = f->version[2];
	rom[0x102] = f->version[1];
	rom[0x103] = f->version[0];
	rom[0x104] = f->version[4];
	token += stride;
	token[0] = 'P';
	token[1] = 2;
	write16(token + 2, 0x30);
	write16(token + 4, f->token);
	write32(rom + f->token, 0x800);
	write32(rom + f->token + 0x2c, table);
	token += stride;
	token[0] = 'C';
	token[1] = 2;
	write16(token + 2, 0x10);
	write16(token + 4, 0x220);
	write32(rom + 0x228, 0x1a00);
	write32(rom + 0x22c, 0x1b00);
	memcpy(rom + f->clocks, "\x35\x09\x0b\x0a\x03\xe8\x03\x00\x00", 9);
	rom[f->clocks + 9] = 1;
	rom[f->clocks + 10] = 1;
	rom[f->clocks + 11] = 2; /* Two GPC programs, neither at index zero. */
	memcpy(rom + f->programs, "\x35\x0b\x0d\x03\x03\x04\x02\x01\x02\x01\x00", 11);
	rom[f->programs + 11 + 29] = 0xa0;
	rom[f->programs + 11 + 58] = 0xa0;
	memcpy(rom + f->power, "\x30\x2c\x43\x04", 4);
	rom[f->power + 10] = tgp;
	rom[f->entry] = 1;
	write32(rom + f->entry + 2, minimum);
	write32(rom + f->entry + 6, rated);
	write32(rom + f->entry + 10, maximum);
	memcpy(rom + f->perf, "\x60\x0a\x05\x04\x06\x0a\x00\x0f\x00\x01", 10);
	for (unsigned int i = 0; i < 4; i++)
		rom[f->perf + 10 + 65 * i] = 0xff;
	p0 = rom + f->perf + 10 + 65 * 3;
	memcpy(p0, "\x0f\x08\x03\x00\x00", 5);
	for (unsigned int i = 0; i < 10; i++) {
		write16(p0 + 5 + 6 * i, floors[i]);
		write32(p0 + 7 + 6 * i, floors[i] | (u32)floors[i] << 14);
	}
	checksum(f);
}

static void apply(struct fixture *f, const u8 *record, size_t size)
{
	u32 offset = read32(record + 5);
	assert(size == (size_t)record[9] + 10);
	assert(!memcmp(record, f->version, 5));
	assert(offset + record[9] <= f->end);
	memcpy(f->rom + f->base + offset, record + 10, record[9]);
}

static void supported(void)
{
	for (unsigned int variant = 0; variant < 8; variant++) {
		struct fixture original, working, applied;
		u8 power[22], check[11], offset[14], offset_check[11], p8[75], p8_check[11];
		u32 minimum = variant & 1 ? 150000 : 100000;
		u32 rated = variant & 1 ? 300000 : 250000;
		u32 maximum = variant & 1 ? 300000 : 250000;
		u32 wanted_min = variant & 1 ? 100000 : 0;
		u32 wanted_max = variant & 1 ? 0 : 300000;

		fixture(&original, variant & 1, variant & 2, variant & 4 ? 8 : 6,
			variant & 4 ? 0x1500 : 0x1000, variant % 4, minimum, rated, maximum);
		working = applied = original;
		assert(!gpu_unlock_vbios_power_overrides(working.rom, sizeof(working.rom),
			wanted_min, wanted_max, power, check));
		assert(read32(power + 5) == original.entry + 2 && power[9] == 12);
		assert(read32(power + 10) == 100000);
		assert(read32(power + 14) == rated);
		assert(read32(power + 18) == 300000);
		apply(&applied, power, sizeof(power));
		apply(&applied, check, sizeof(check));
		assert(!memcmp(working.rom, applied.rom, sizeof(working.rom)));
		for (size_t i = 0; i < sizeof(working.rom); i++) {
			u32 at = original.base + original.entry + 2;
			if (i >= at && i < at + 12)
				continue;
			if (i == original.base + original.end - 1)
				continue;
			assert(working.rom[i] == original.rom[i]);
		}
		assert(!gpu_unlock_vbios_gpc_offset_overrides(working.rom, sizeof(working.rom),
			1000, offset, offset_check));
		assert(read32(offset + 5) == original.clocks + 12 && offset[9] == 4);
		assert(!memcmp(offset + 10, "\x18\xfc\xe8\x03", 4));
		apply(&applied, offset, sizeof(offset));
		apply(&applied, offset_check, sizeof(offset_check));
		assert(!memcmp(working.rom, applied.rom, sizeof(working.rom)));
		assert(gpu_unlock_vbios_gpc_offset_overrides(working.rom, sizeof(working.rom),
			1000, offset, offset_check) == -EALREADY);
		assert(!gpu_unlock_vbios_p8_overrides(working.rom, sizeof(working.rom),
			135, p8, p8_check));
		apply(&applied, p8, sizeof(p8));
		apply(&applied, p8_check, sizeof(p8_check));
		assert(!sum(applied.rom + original.base + 0x400 + original.adjust, 0x2000));
		assert(read32(applied.rom + original.base + original.entry + 2) == 100000);
		assert(read32(applied.rom + original.base + original.entry + 6) == rated);
		assert(read32(applied.rom + original.base + original.entry + 10) == 300000);
		assert(!memcmp(applied.rom + original.base + original.clocks + 12,
			"\x18\xfc\xe8\x03", 4));
		assert(!memcmp(applied.rom + original.base + original.perf + 10 + 65 * 3,
			original.rom + original.base + original.perf + 10 + 65 * 3, 65));
		assert(gpu_unlock_vbios_power_overrides(working.rom, sizeof(working.rom),
			wanted_min, wanted_max, power, check) == -EALREADY);
	}
}

static void preserved(void)
{
	struct fixture f, original;
	u8 power[22], check[11], p8[75], p8_check[11];

	fixture(&f, false, false, 6, 0x1000, 2, 75000, 300000, 350000);
	original = f;
	assert(gpu_unlock_vbios_power_overrides(f.rom, sizeof(f.rom), 100000, 300000,
		power, check) == -EALREADY);
	assert(!memcmp(&f, &original, sizeof(f)));
	fixture(&f, false, false, 6, 0x1000, 2, 150000, 250000, 250000);
	/* Both bounds fit in the same power record. A factory P8 is retained. */
	f.rom[f.perf + 10] = 7;
	checksum(&f);
	assert(!gpu_unlock_vbios_power_overrides(f.rom, sizeof(f.rom), 100000, 300000,
		power, check));
	assert(read32(power + 14) == 250000);
	assert(gpu_unlock_vbios_p8_overrides(f.rom, sizeof(f.rom), 135, p8, p8_check) == -EALREADY);
	assert(!sum(f.rom + 0x400, 0x2000));
}

static void rejected(void)
{
	struct fixture valid, f, before;
	u8 power[22], check[11];

	fixture(&valid, true, true, 8, 0x1000, 2, 150000, 300000, 300000);
	for (unsigned int variant = 0; variant < 13; variant++) {
		size_t length;
		u8 *rom;
		f = valid;
		rom = f.rom + f.base;
		length = sizeof(f.rom);
		switch (variant) {
		case 0: rom[f.power] = 0x31; break;
		case 1: rom[f.power + 1] = 0x2b; break;
		case 2: rom[f.power + 2] = 0x47; break;
		case 3: rom[f.power + 10] = 0xff; break;
		case 4: rom[f.entry] = 0x11; break; /* Current units. */
		case 5: rom[f.entry + 0x42] = 1; break; /* Extended class. */
		case 6: write32(rom + f.entry + 2, 350000); break;
		case 7: write32(rom + f.entry + 10, 250000); break;
		case 8: write32(rom + f.entry + 2, 0); break;
		case 9: write32(rom + f.token + 0x2c, 0xffffffff); break;
		case 10: write32(rom + f.token + 0x2c, 0); break;
		case 11: length = f.base + f.entry + 10; break;
		case 12: rom[f.end - 1]++; break;
		}
		before = f;
		assert(gpu_unlock_vbios_power_overrides(f.rom, length, 100000, 0, power, check) < 0);
		assert(!memcmp(&f, &before, sizeof(f)));
	}
	f = before = valid;
	assert(gpu_unlock_vbios_power_overrides(f.rom, sizeof(f.rom), 0, 0, power, check) == -EINVAL);
	assert(gpu_unlock_vbios_power_overrides(f.rom, sizeof(f.rom), 300000, 100000, power, check) == -EINVAL);
	assert(!memcmp(&f, &before, sizeof(f)));
}

static void offset_preserved(void)
{
	struct fixture f, before;
	u8 offset[14], check[11], p8[75], p8_check[11];

	fixture(&f, false, false, 6, 0x1000, 2, 100000, 250000, 300000);
	for (unsigned int i = 0; i < 3; i++) {
		/* Native, asymmetric and negative-only factory ranges are retained. */
		write16(f.rom + f.clocks + 12, i == 0 ? (u16)-1000 : (u16)-250);
		write16(f.rom + f.clocks + 14, i == 0 ? 1000 : i == 1 ? 500 : 0);
		checksum(&f);
		before = f;
		assert(gpu_unlock_vbios_gpc_offset_overrides(f.rom, sizeof(f.rom),
			1000, offset, check) == -EALREADY);
		assert(!memcmp(&f, &before, sizeof(f)));
	}
	write32(f.rom + f.clocks + 12, 0);
	f.rom[f.perf + 10] = 7; /* Existing P8 does not block the offset change. */
	checksum(&f);
	assert(!gpu_unlock_vbios_gpc_offset_overrides(f.rom, sizeof(f.rom), 1000, offset, check));
	assert(gpu_unlock_vbios_p8_overrides(f.rom, sizeof(f.rom), 135, p8, p8_check) == -EALREADY);
	assert(!sum(f.rom + 0x400, 0x2000));
}

static void offset_rejected(void)
{
	struct fixture valid, f, before;
	u8 offset[14], check[11];

	fixture(&valid, true, true, 8, 0x1500, 2, 150000, 300000, 300000);
	for (unsigned int variant = 0; variant < 21; variant++) {
		u8 *rom;
		size_t length = sizeof(f.rom);
		f = valid;
		rom = f.rom + f.base;
		switch (variant) {
		case 0: rom[f.clocks] = 0x40; break;
		case 1: rom[f.clocks + 1] = 7; break;
		case 2: rom[f.clocks + 2] = 9; break;
		case 3: rom[f.clocks + 3] = 9; break;
		case 4: rom[f.clocks + 4] = 4; break;
		case 5: rom[f.clocks + 9] = 0; break;
		case 6: rom[f.clocks + 9] = 2; break;
		case 7: rom[f.clocks + 10] = 3; break;
		case 8: rom[f.clocks + 11] = 3; break;
		case 9: rom[f.programs + 11 + 58] &= ~0x80; break;
		case 10: rom[f.programs + 11 + 29] = 0xaf; break;
		case 11: rom[f.perf + 10 + 65 * 3 + 1] &= ~8; break;
		case 12: rom[f.perf + 10 + 65 * 3] = 0xff; break;
		case 13: rom[f.perf + 10] = 15; rom[f.perf + 11] = 8; break;
		case 14: write16(rom + f.clocks + 12, 100); break;
		case 15: write16(rom + f.clocks + 14, (u16)-100); break;
		case 16: write32(rom + 0x228, 0xffffffff); break;
		case 17: write32(rom + 0x22c, 0); break;
		case 18: rom[f.programs + 2] = 5; break;
		case 19: length = f.base + f.programs + 20; break;
		case 20: rom[f.end - 1]++; break;
		}
		before = f;
		assert(gpu_unlock_vbios_gpc_offset_overrides(f.rom, length, 1000, offset, check) < 0);
		assert(!memcmp(&f, &before, sizeof(f)));
	}
	f = before = valid;
	assert(gpu_unlock_vbios_gpc_offset_overrides(f.rom, sizeof(f.rom), 0, offset, check) == -EINVAL);
	assert(gpu_unlock_vbios_gpc_offset_overrides(f.rom, sizeof(f.rom), 0x8000, offset, check) == -EINVAL);
	assert(!memcmp(&f, &before, sizeof(f)));
}

int main(void)
{
	supported();
	preserved();
	rejected();
	offset_preserved();
	offset_rejected();
	puts("VBIOS power/core-offset overrides: relocated tables, OC gates, factory ranges and P8 composition passed");
	return 0;
}
