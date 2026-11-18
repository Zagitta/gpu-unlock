// SPDX-License-Identifier: GPL-2.0-only
#include "diagnostic.h"
/* Formats: NVIDIA 610.43.03 kernel_gsp_fwsec.c and kernel_gsp_vbios_tu102.c.
 * The PROM contains the full VBIOS container; locate the IFR image offset the
 * same way kgspExtractVbiosFromRom_TU102 does before parsing the legacy image.
 * Independent byte parser; no firmware-owned pointer is trusted as a host pointer.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#endif
#include "vbios.h"

/* Production selects 0x85. The integration build selects the debug app
 * 0x45; this is a compile-time seam, never a runtime knob. */
#ifndef GPU_UNLOCK_FWSEC_APP
#define GPU_UNLOCK_FWSEC_APP 0x85
#endif

static u16 r16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 r32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void w32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static int range(size_t length, u64 off, u64 bytes)
{
	return off > length || bytes > length - off ? -EINVAL : 0;
}

/* kgspExtractVbiosFromRom_TU102 / s_locateExpansionRoms. Image signatures are
 * 0xAA55, 0x4E56 ("VN") and 0xBB77. Data signatures are PCIR, NPDS and RGIS.
 */
static int rom_signature(u16 sig)
{
	return sig == 0xaa55 || sig == 0x4e56 || sig == 0xbb77;
}

static int data_signature(u32 sig)
{
	return sig == 0x52494350 || sig == 0x5344504e || sig == 0x53494752;
}

static int expansion(const u8 *rom, size_t length, u32 *adjust)
{
	u32 pos = 0, base = 0, ext = 0;
	unsigned int images = 0;

	if (range(length, 0, 2) || !rom_signature(r16(rom)))
		return GU_DIAG(-EINVAL);
	for (;;) {
		u64 pcir, npde;
		u32 bytes, last, sig;
		u16 ext_rev, ext_len;

		if (++images > 8 || range(length, pos, 0x1a) || !rom_signature(r16(rom + pos)))
			return GU_DIAG(-EINVAL);
		pcir = (u64)pos + r16(rom + pos + 0x18);
		if (range(length, pcir, 0x16))
			return GU_DIAG(-EINVAL);
		sig = r32(rom + pcir);
		if (!data_signature(sig))
			return GU_DIAG(-EINVAL);
		bytes = (u32)r16(rom + pcir + 0x10) * 512;
		last = rom[pcir + 0x15] & 0x80;
		npde = (pcir + r16(rom + pcir + 0x0a) + 15) & ~(u64)15;
		if (!range(length, npde, 4) && r32(rom + npde) == 0x4544504e) {
			if (range(length, npde, 10))
				return GU_DIAG(-EINVAL);
			ext_rev = r16(rom + npde + 4);
			ext_len = r16(rom + npde + 6);
			if (ext_rev == 0x100 || ext_rev == 0x101) {
				u32 sub = (u32)r16(rom + npde + 8) * 512;

				if (ext_len < 10 || range(length, npde, ext_len))
					return GU_DIAG(-EINVAL);
				if (ext_len > 10)
					last = rom[npde + 10] & 0x80;
				else if (sub < bytes)
					last = 0;
				bytes = sub;
			}
		}
		GU_NOTE("ROM chain: pos=%#x data=%#x bytes=%u type=%#x last=%u\n",
			pos, sig, bytes, rom[pcir + 0x14], last);
		if (!bytes || range(length, pos, bytes))
			return GU_DIAG(-EINVAL);
		if (!base && rom[pcir + 0x14] == 0)
			base = bytes;
		if (!ext && rom[pcir + 0x14] == 0xe0)
			ext = pos;
		if (last)
			break;
		pos += bytes;
	}
	if (ext && base && ext < base)
		return GU_DIAG(-EINVAL);
	*adjust = ext && base ? ext - base : 0;
	GU_NOTE("ROM expansion: base=%#x ext=%#x adjust=%#x\n", base, ext, *adjust);
	return 0;
}

static int descriptor(const u8 *rom, size_t length, u64 off,
		      struct gpu_unlock_fwsec *out)
{
	struct gpu_unlock_fwsec fw = { 0 };
	u32 hdr, size, stored, bits, count = 0;

	if (range(length, off, 44))
		return GU_DIAG(-EINVAL);
	hdr = r32(rom + off);
	size = hdr >> 16;
	GU_NOTE("FWSEC descriptor: offset=%#llx ROM_bytes=%zu header=%#x\n",
		(unsigned long long)off, length, hdr);
	fw.version = (hdr >> 8) & 0xff;
	/* v2 generic-loader descriptor. TU102 and GA100 share this layout, but
	 * TU102 uses a non-zero secure IMEM base inside the same image. */
	if ((hdr & 1) && fw.version == 2) {
		u32 sec_base, sec_size, code_size_aligned, data_size_aligned;

		if (size < 60 || range(length, off, size))
			return GU_DIAG(-EINVAL);
		/* The generic loader starts at zero and loads DMEM at zero. */
		if (r32(rom + off + 12) || r32(rom + off + 44))
			return GU_DIAG(-EINVAL);
		stored = r32(rom + off + 4);
		if (!stored || stored > ~(u32)0 - 255)
			return GU_DIAG(-EINVAL);
		fw.image_size = (stored + 255) & ~255u;
		if (range(length, off + size, fw.image_size))
			return GU_DIAG(-EINVAL);
		fw.image_off = off + size;
		fw.interface_off = r32(rom + off + 16);
		fw.imem_pa = r32(rom + off + 20);
		fw.imem_size = r32(rom + off + 24);
		fw.imem_va = r32(rom + off + 28);
		sec_base = r32(rom + off + 32);
		sec_size = r32(rom + off + 36);
		if (sec_size > fw.imem_size || sec_size > ~(u32)0 - 255 ||
		    sec_base < fw.imem_va ||
		    sec_base - fw.imem_va > ~(u32)0 - fw.imem_pa)
			return GU_DIAG(-EINVAL);
		fw.imem_sec_size = (sec_size + 255) & ~255u;
		fw.imem_ns_size = fw.imem_size - sec_size;
		fw.imem_sec_pa = sec_base - fw.imem_va + fw.imem_pa;
		fw.data_off = r32(rom + off + 40);
		fw.dmem_pa = r32(rom + off + 44);
		fw.dmem_size = r32(rom + off + 48);
		code_size_aligned = (fw.imem_size + 255) & ~255u;
		data_size_aligned = (fw.dmem_size + 255) & ~255u;
		if (!fw.imem_size || !fw.dmem_size ||
		    range(fw.image_size, 0, fw.imem_size) ||
		    range(fw.image_size, fw.imem_sec_pa, fw.imem_sec_size) ||
		    range(fw.image_size, fw.data_off, fw.dmem_size) ||
		    range(code_size_aligned, fw.imem_pa, fw.imem_size) ||
		    range(code_size_aligned, fw.imem_sec_pa, fw.imem_sec_size) ||
		    range(data_size_aligned, fw.dmem_pa, fw.dmem_size) ||
		    range(fw.dmem_size, fw.interface_off, 4))
			return GU_DIAG(-EINVAL);
		*out = fw;
		return 0;
	}
	/* vDesc: bit 0 set, version in bits 15:8, size in bits 31:16. */
	if (!(hdr & 1) || ((hdr >> 8) & 0xff) != 3 || size < 44)
		return GU_DIAG(-EOPNOTSUPP);
	if (range(length, off, size))
		return GU_DIAG(-EINVAL);
	stored = r32(rom + off + 4);
	if (!stored || stored > ~(u32)0 - 255)
		return GU_DIAG(-EINVAL);
	fw.image_size = (stored + 255) & ~255u;
	if (range(length, off + size, fw.image_size))
		return GU_DIAG(-EINVAL);
	fw.image_off = off + size;
	fw.pkc_off = r32(rom + off + 8);
	fw.interface_off = r32(rom + off + 12);
	fw.imem_pa = r32(rom + off + 16);
	fw.imem_size = r32(rom + off + 20);
	fw.imem_va = r32(rom + off + 24);
	fw.dmem_pa = r32(rom + off + 28);
	fw.dmem_size = r32(rom + off + 32);
	fw.data_off = fw.imem_size;
	fw.engine_mask = r16(rom + off + 36);
	fw.ucode_id = rom[off + 38];
	fw.sig_count = rom[off + 39];
	fw.sig_versions = r16(rom + off + 40);
	fw.sig_off = off + 44;
	fw.sig_bytes = size - 44;
	GU_NOTE("FWSEC geometry: image_off=%#x size=%#x imem=%#x va=%#x pa=%#x dmem=%#x pa=%#x pkc=%#x interface=%#x engine=%#x ucode=%u signatures=%u versions=%#x bytes=%u\n",
		fw.image_off, fw.image_size, fw.imem_size, fw.imem_va, fw.imem_pa,
		fw.dmem_size, fw.dmem_pa, fw.pkc_off, fw.interface_off, fw.engine_mask,
		fw.ucode_id, fw.sig_count, fw.sig_versions, fw.sig_bytes);
	bits = fw.sig_versions;
	while (bits) {
		count += bits & 1;
		bits >>= 1;
	}
	if (!fw.ucode_id || fw.ucode_id > 16 || !fw.engine_mask || !count ||
	    count != fw.sig_count || fw.sig_bytes != count * 384 ||
	    !fw.imem_size || !fw.dmem_size ||
	    range(fw.image_size, fw.data_off, fw.dmem_size) ||
	    range(fw.image_size, fw.data_off, ((u64)fw.dmem_size + 255) & ~255ull) ||
	    range(fw.dmem_size, fw.pkc_off, 384) ||
	    range(fw.dmem_size, fw.interface_off, 4))
		return GU_DIAG(-EINVAL);
	*out = fw;
	return 0;
}

static int pci_image(const u8 *rom, size_t length, u32 *offset)
{
	u32 image = 0;

	/* The GPU exposes the complete VBIOS container through the PROM
	 * window.  NVIDIA's kgspExtractVbiosFromRom_TU102 walks the IFR
	 * header to the first PCI expansion image; match that here instead of
	 * requiring the container to start with 0xaa55. */
	if (length >= 12 && r32(rom) == 0x4947564eu) {
		u32 version = (r32(rom + 4) >> 8) & 0xffu;
		if (version != 3)
			return GU_DIAG(-EOPNOTSUPP);
		{
			u32 total = r32(rom + 8) & 0xfffffu;
			u32 flash;
			u64 dir;
			if (range(length, total, 4))
				return GU_DIAG(-EINVAL);
			flash = r32(rom + total);
			dir = (u64)flash + 0x1000u;
			if (range(length, dir, 12) || r32(rom + dir) != 0x44524652u)
				return GU_DIAG(-EINVAL);
			image = r32(rom + dir + 8);
		}
		if (!image || (image & 3u) || range(length, image, 2) ||
		    r16(rom + image) != 0xaa55u)
			return GU_DIAG(-EINVAL);
		GU_NOTE("ROM image: container NVGI version 3 image=%#x\n", image);
	}
	*offset = image;
	return 0;
}

int gpu_unlock_vbios_fwsec(const u8 *rom, size_t length, struct gpu_unlock_fwsec *out)
{
	u32 bit, adjust, image = 0;
	int ret;

	if (!rom || !out || length > 1024 * 1024)
		return GU_DIAG(-EINVAL);
	memset(out, 0, sizeof(*out));

	ret = pci_image(rom, length, &image);
	if (ret)
		return GU_DIAG(ret);
	rom += image;
	length -= image;

	ret = expansion(rom, length, &adjust);
	if (ret)
		return GU_DIAG(ret);
	for (bit = 0; bit + 12 <= length; bit++) {
		u32 size, stride, tokens, sum = 0, i;

		if (r16(rom + bit) != 0xb8ff || r32(rom + bit + 2) != 0x00544942)
			continue;
		size = rom[bit + 8];
		stride = rom[bit + 9];
		tokens = rom[bit + 10];
		if (size < 12 || stride < 6 || range(length, bit, size))
			return GU_DIAG(-EINVAL);
		for (i = 0; i < size; i++)
			sum += rom[bit + i];
		if (sum & 0xff)
			continue;
		GU_NOTE("ROM BIT: offset=%#x size=%u stride=%u tokens=%u adjust=%#x\n",
			bit, size, stride, tokens, adjust);
		if (range(length, (u64)bit + size, (u64)tokens * stride))
			return GU_DIAG(-EINVAL);
		for (i = 0; i < tokens; i++) {
			const u8 *token = rom + bit + size + i * stride;
			u32 ptr, j;
			u64 table;

			if (token[0] != 0x70 || token[1] != 2 || r16(token + 2) < 4)
				continue;
			ptr = stride >= 8 ? r32(token + 4) : r16(token + 4);
			if (range(length, ptr, r16(token + 2)))
				return GU_DIAG(-EINVAL);
			table = (u64)adjust + r32(rom + ptr);
			if (range(length, table, 6))
				return GU_DIAG(-EINVAL);
			GU_NOTE("ROM ucode table: offset=%#llx version=%u header=%u stride=%u entries=%u\n",
				(unsigned long long)table, rom[table], rom[table+1], rom[table+2], rom[table+3]);
			if (rom[table] != 1 || rom[table + 1] < 6 || rom[table + 2] < 6)
				return GU_DIAG(-EOPNOTSUPP);
			if (range(length, table + rom[table + 1],
				  (u64)rom[table + 2] * rom[table + 3]))
				return GU_DIAG(-EINVAL);
			for (j = 0; j < rom[table + 3]; j++) {
				const u8 *entry = rom + table + rom[table + 1] + j * rom[table + 2];

				if (entry[0] == GPU_UNLOCK_FWSEC_APP) {
					ret = descriptor(rom, length,
							  (u64)adjust + r32(entry + 2), out);
					if (!ret) {
						out->image_off += image;
						if (out->sig_off)
							out->sig_off += image;
					}
					return GU_DIAG(ret);
				}
			}
		}
	}
	return GU_DIAG(-ENOENT);
}

struct override_image {
	const u8 *rom;
	size_t length;
	u32 adjust, token;
	u8 version[5];
};

static int override_image(const u8 *rom, size_t length, u8 kind, u32 minimum,
			  struct override_image *out)
{
	u32 image, adjust, pcir, bit, legacy, bios = 0, data = 0;
	bool found = false;
	int ret;

	if (!rom || length > 1024 * 1024)
		return -EINVAL;
	ret = pci_image(rom, length, &image);
	if (ret)
		return ret;
	rom += image;
	length -= image;
	ret = expansion(rom, length, &adjust);
	if (ret || range(length, 0, 0x1a))
		return ret ? ret : -EINVAL;
	pcir = r16(rom + 0x18);
	if (range(length, pcir, 0x16) || r16(rom) != 0xaa55 ||
	    r32(rom + pcir) != 0x52494350 || r16(rom + pcir + 4) != 0x10de ||
	    rom[pcir + 0x14] != 0)
		return -EOPNOTSUPP;
	/* BIT is in the legacy image; extended tables can live in NPDS images. */
	if (!r16(rom + pcir + 0x10) ||
	    range(length, 0, (size_t)r16(rom + pcir + 0x10) * 512))
		return -EINVAL;
	legacy = (u32)r16(rom + pcir + 0x10) * 512;
	for (bit = 0; bit + 12 <= legacy; bit++) {
		u32 header, stride, count, sum = 0, i, b = 0, in = 0;

		if (r16(rom + bit) != 0xb8ff || r32(rom + bit + 2) != 0x00544942)
			continue;
		header = rom[bit + 8];
		stride = rom[bit + 9];
		count = rom[bit + 10];
		if (r16(rom + bit + 6) != 0x100 || header < 12 ||
		    (stride != 6 && stride != 8) || range(legacy, bit, header) ||
		    range(legacy, (u64)bit + header, (u64)count * stride))
			return -EBADMSG;
		for (i = 0; i < header; i++)
			sum += rom[bit + i];
		if (sum & 0xff)
			return -EBADMSG;
		for (i = 0; i < count; i++) {
			const u8 *token = rom + bit + header + i * stride;
			u32 ptr = stride == 8 ? r32(token + 4) : r16(token + 4);

			if (token[0] != 'B' && token[0] != kind)
				continue;
			if (token[1] != 2 || range(legacy, ptr, r16(token + 2)) || !ptr)
				return -EOPNOTSUPP;
			if (token[0] == 'B') {
				if (b || r16(token + 2) < 5)
					return -EBADMSG;
				b = ptr;
			} else {
				if (in || r16(token + 2) < minimum)
					return -EBADMSG;
				in = ptr;
			}
		}
		if (!b || !in || found)
			return -EBADMSG;
		bios = b;
		data = in;
		found = true;
	}
	if (!found)
		return -ENOENT;
	/* Zero is a wildcard in NVIDIA's version guards; never emit one. */
	if (!r32(rom + bios) || !rom[bios + 4])
		return -EOPNOTSUPP;
	*out = (struct override_image) {
		.rom = rom, .length = length, .adjust = adjust, .token = data,
		.version = { rom[bios + 3], rom[bios + 2], rom[bios + 1], rom[bios], rom[bios + 4] },
	};
	return 0;
}

static void override_record(const struct override_image *image, u32 offset,
			    const u8 *data, u8 size, u8 *record)
{
	memcpy(record, image->version, sizeof(image->version));
	w32(record + 5, offset);
	record[9] = size;
	memcpy(record + 10, data, size);
}

static const u8 *override_table(const struct override_image *image, u32 pointer, u32 bytes)
{
	u32 offset = r32(image->rom + image->token + pointer);

	if (!offset || offset > ~(u32)0 - image->adjust)
		return NULL;
	offset += image->adjust;
	return range(image->length, offset, bytes) ? NULL : image->rom + offset;
}

int gpu_unlock_vbios_ecc_override(const u8 *rom, size_t length, u8 record[11])
{
	struct override_image image;
	u8 value;
	int ret;

	if (!record)
		return -EINVAL;
	ret = override_image(rom, length, 'i', 0x44, &image);
	if (ret)
		return ret;
	value = image.rom[image.token + 0x43];
	if (value & 1)
		return -EALREADY;
	value |= 1;
	override_record(&image, image.token + 0x43, &value, 1, record);
	return 0;
}

/* Find the PCI/NPDS image owning the entire edit, including its checksum.
 * Extended images are 512-byte aligned relative to the legacy image. */
static int override_checksum(const struct override_image *image, u32 offset,
			      const u8 *data, u8 size, u8 record[11])
{
	const u8 *rom = image->rom;
	u32 pos, pcir, bytes, sum, i;
	u8 checksum;

	for (pos = 0; pos <= offset; pos += 512) {
		if (range(image->length, pos, 0x1a) || !rom_signature(r16(rom + pos)))
			continue;
		pcir = pos + r16(rom + pos + 0x18);
		if (range(image->length, pcir, 0x16) || !data_signature(r32(rom + pcir)))
			continue;
		bytes = (u32)r16(rom + pcir + 0x10) * 512;
		if (!bytes || range(image->length, pos, bytes) || offset < pos ||
		    range(bytes - 1, offset - pos, size))
			continue;
		sum = 0;
		for (i = 0; i < bytes; i++)
			sum += rom[pos + i];
		if (sum & 0xff)
			return -EBADMSG;
		for (i = 0; i < size; i++)
			sum += data[i] - rom[offset + i];
		checksum = rom[pos + bytes - 1] - sum;
		override_record(image, pos + bytes - 1, &checksum, 1, record);
		return 0;
	}
	return -EINVAL;
}

/* Apply only generated records to the captured RAM copy. Each later edit
 * starts from a valid image checksum including the preceding edits. */
static int override_patch(u8 *rom, const struct override_image *image, u32 offset,
			  const u8 *data, u8 size, u8 *record, u8 checksum[11])
{
	size_t base = image->rom - rom;
	int ret = override_checksum(image, offset, data, size, checksum);

	if (ret)
		return ret;
	override_record(image, offset, data, size, record);
	memcpy(rom + base + offset, data, size);
	rom[base + r32(checksum + 5)] = checksum[10];
	return 0;
}

int gpu_unlock_vbios_power_overrides(u8 *rom, size_t length, u32 minimum_mw,
				    u32 maximum_mw, u8 record[22], u8 checksum[11])
{
	struct override_image image;
	const u8 *table, *entry;
	u32 offset, minimum, rated, maximum;
	u8 limits[12];
	int ret;

	if (!record || !checksum || (!minimum_mw && !maximum_mw) ||
	    (minimum_mw && maximum_mw && minimum_mw > maximum_mw))
		return -EINVAL;
	ret = override_image(rom, length, 'P', 0x30, &image);
	if (ret)
		return ret;
	table = override_table(&image, 0x2c, 0x2c);
	if (!table)
		return -EINVAL;
	offset = table - image.rom;
	/* Reviewed GA100 power-policy 0x30 layout. The header selects TGP;
	 * neither its row index nor the table's ROM offset is fixed. */
	if (table[0] != 0x30 || table[1] != 0x2c || table[2] != 0x43 ||
	    table[10] >= table[3] ||
	    range(image.length, offset + 0x2c, (u32)table[2] * table[3]))
		return -EOPNOTSUPP;
	offset += table[1] + (u32)table[2] * table[10];
	entry = image.rom + offset;
	/* TOTAL_GPU policy, power units (mW), including the extended class. */
	if ((entry[0] & 0x1f) != 1 || (entry[0x42] & 0xf))
		return -EOPNOTSUPP;
	minimum = r32(entry + 2);
	rated = r32(entry + 6);
	maximum = r32(entry + 10);
	if (!minimum || minimum > rated || rated > maximum)
		return -EBADMSG;
	if (minimum_mw && minimum_mw < minimum)
		minimum = minimum_mw;
	if (maximum_mw && maximum_mw > maximum)
		maximum = maximum_mw;
	w32(limits, minimum);
	w32(limits + 4, rated);
	w32(limits + 8, maximum);
	if (!memcmp(entry + 2, limits, sizeof(limits)))
		return -EALREADY;
	return override_patch(rom, &image, offset + 2, limits, sizeof(limits), record, checksum);
}

int gpu_unlock_vbios_gpc_offset_overrides(u8 *rom, size_t length, u16 offset_mhz,
					 u8 record[14], u8 checksum[11])
{
	struct override_image image, perf;
	const u8 *clocks, *gpc, *programs, *table, *entry;
	u32 offset, stride, i, p0 = 0;
	u8 bounds[4];
	int ret;

	if (!record || !checksum || !offset_mhz || offset_mhz > 0x7fff)
		return -EINVAL;
	ret = override_image(rom, length, 'C', 0x10, &image);
	if (ret)
		return ret;
	clocks = override_table(&image, 8, 9);
	if (!clocks)
		return -EINVAL;
	/* HAL 3 identifies row zero as GPC; MASTER PARAM1 holds two signed
	 * MHz bounds. FIXED and SLAVE rows have different PARAM1 meanings. */
	if (memcmp(clocks, "\x35\x09\x0b\x0a\x03", 5) ||
	    range(image.length, clocks - image.rom + 9, 10 * 11))
		return -EOPNOTSUPP;
	gpc = clocks + 9;
	if ((gpc[0] & 0x1f) != 1)
		return -EOPNOTSUPP;
	programs = override_table(&image, 12, 11);
	if (!programs)
		return -EINVAL;
	if (programs[0] != 0x35 || programs[1] != 11 || programs[2] != 13 ||
	    memcmp(programs + 4, "\x03\x04\x02\x01\x02\x01", 6) ||
	    gpc[1] > gpc[2] || gpc[2] >= programs[3])
		return -EOPNOTSUPP;
	stride = programs[2] + programs[4] * programs[5] +
		 programs[7] * (programs[6] + programs[8] * programs[9]);
	if (range(image.length, programs - image.rom + programs[1],
		  (u32)programs[3] * stride))
		return -EINVAL;
	for (i = gpc[1]; i <= gpc[2]; i++) {
		entry = programs + programs[1] + i * stride;
		/* The client VF interface also requires OC-enabled MASTER programs. */
		if ((entry[0] & 0xf) > 1 || !(entry[0] & 0x80))
			return -EOPNOTSUPP;
	}
	ret = override_image(rom, length, 'P', 4, &perf);
	if (ret)
		return ret;
	table = override_table(&perf, 0, 10);
	if (!table)
		return -EINVAL;
	if (table[0] != 0x60 || table[1] != 10 || table[2] != 5 ||
	    !table[3] || table[4] != 6 || table[5] != 10 ||
	    range(perf.length, table - perf.rom + 10, (u32)table[3] * 65))
		return -EOPNOTSUPP;
	for (i = 0; i < table[3]; i++) {
		entry = table + 10 + i * 65;
		if (entry[0] != 15)
			continue;
		if (p0++)
			return -EBADMSG;
		if (!(entry[1] & 8))
			return -EOPNOTSUPP;
	}
	if (!p0)
		return -EOPNOTSUPP;
	if ((r16(gpc + 3) && !(r16(gpc + 3) & 0x8000)) || (r16(gpc + 5) & 0x8000))
		return -EBADMSG;
	if (r32(gpc + 3))
		return -EALREADY;
	w32(bounds, (0x10000u - offset_mhz) | ((u32)offset_mhz << 16));
	offset = gpc - image.rom + 3;
	return override_patch(rom, &image, offset, bounds, sizeof(bounds), record, checksum);
}

int gpu_unlock_vbios_p8_overrides(const u8 *rom, size_t length, u16 minimum_mhz,
				 u8 record[75], u8 checksum[11])
{
	static const u8 domains[] = { 0, 1, 3, 5, 9 }; /* GPC, XBAR, SYS, NVD, HOST */
	struct override_image image;
	const u8 *table;
	u32 offset, entry, p0 = 0, empty = 0, i;
	u8 row[65];
	int ret;

	if (!record || !checksum || !minimum_mhz || minimum_mhz > 0x3fff)
		return -EINVAL;
	ret = override_image(rom, length, 'P', 4, &image);
	if (ret)
		return ret;
	table = override_table(&image, 0, 10);
	if (!table)
		return -EINVAL;
	offset = table - image.rom;
	/* Only the inspected GA100 performance 0x60 layout is supported. */
	if (table[0] != 0x60 || table[1] != 10 || table[2] != 5 ||
	    !table[3] || table[4] != 6 || table[5] != 10 ||
	    range(image.length, offset + 10, (u32)table[3] * sizeof(row)))
		return -EOPNOTSUPP;
	for (i = 0; i < table[3]; i++) {
		entry = offset + 10 + i * sizeof(row);
		if (image.rom[entry] == 7)
			return -EALREADY; /* Keep a factory P8 intact. */
		if (image.rom[entry] == 0xff && !empty)
			empty = entry;
		if (image.rom[entry] == 15) {
			if (p0)
				return -EBADMSG;
			p0 = entry;
		}
	}
	/* RM's idle limit uses insertion level zero, so do not put P8 after
	 * an existing state or reorder the factory entries. */
	if (!p0 || empty != offset + 10 || (memcmp(image.rom + p0, "\x0f\x08\x03\x00\x00", 5) &&
	    memcmp(image.rom + p0, "\x0f\x08\x03\xff\x00", 5)))
		return -EOPNOTSUPP;
	memcpy(row, image.rom + p0, sizeof(row));
	/* Reuse the tested P0 power/link policies as well as its clock tuples. */
	row[0] = 7;
	/* Put the requested minimum fields in P8. Voltage policy and the
	 * HBM tuple stay intact; the original P0 row is never edited. */
	for (i = 0; i < sizeof(domains); i++) {
		u8 *tuple = row + 5 + domains[i] * 6;
		u32 limits = r32(tuple + 2), minimum = limits & 0x3fff;

		if (!minimum)
			return -EOPNOTSUPP;
		if (minimum > minimum_mhz)
			w32(tuple + 2, (limits & ~0x3fffu) | minimum_mhz);
	}
	ret = override_checksum(&image, empty, row, sizeof(row), checksum);
	if (ret)
		return ret;
	override_record(&image, empty, row, sizeof(row), record);
	return 0;
}

int gpu_unlock_fwsec_sig_offset(u32 versions, u32 fuse_version, u32 sig_size, u32 *offset)
{
	u32 mask, n = 0;

	GU_NOTE("FWSEC signature selection: versions=%#x fuse_version=%u sig_bytes=%u\n",
		versions, fuse_version, sig_size);
	if (!offset || !sig_size || fuse_version >= 16 || versions > 0xffff)
		return GU_DIAG(-EINVAL);
	if (!(versions & (1u << fuse_version)))
		return GU_DIAG(-ERANGE);
	mask = versions & ((1u << fuse_version) - 1);
	while (mask) {
		n += mask & 1;
		mask >>= 1;
	}
	if (n && sig_size > ~(u32)0 / n)
		return GU_DIAG(-EOVERFLOW);
	*offset = n * sig_size;
	return 0;
}

int gpu_unlock_fwsec_patch_frts(u8 *image, const struct gpu_unlock_fwsec *fw, u64 frts_offset)
{
	u8 *dmem, *mapper = NULL;
	u32 entries, i, cmd_off, cmd_size;
	u64 off;
	u8 cmd[44] = { 0 };

	if (!image || !fw || !frts_offset || (frts_offset & 0xfff) ||
	    (frts_offset >> 12) > ~(u32)0 ||
	    range(fw->image_size, fw->data_off, fw->dmem_size) ||
	    range(fw->dmem_size, fw->interface_off, 4))
		return GU_DIAG(-EINVAL);
	dmem = image + fw->data_off;
	GU_NOTE("FWSEC interface: offset=%#x version=%u header=%u stride=%u entries=%u FRTS=%#llx\n",
		fw->interface_off, dmem[fw->interface_off], dmem[fw->interface_off+1],
		dmem[fw->interface_off+2], dmem[fw->interface_off+3], (unsigned long long)frts_offset);
	if (dmem[fw->interface_off] != 1 || dmem[fw->interface_off + 1] != 4 ||
	    dmem[fw->interface_off + 2] != 8)
		return GU_DIAG(-EOPNOTSUPP);
	entries = dmem[fw->interface_off + 3];
	off = (u64)fw->interface_off + 4;
	if (entries < 2 || range(fw->dmem_size, off, (u64)entries * 8))
		return GU_DIAG(-EINVAL);
	for (i = 0; i < entries; i++, off += 8) {
		u32 dest = r32(dmem + off + 4);

		if (r32(dmem + off) == 4) {
			if (mapper || range(fw->dmem_size, dest, 64))
				return GU_DIAG(-EINVAL);
			mapper = dmem + dest;
		}
	}
	if (!mapper)
		return GU_DIAG(-ENOENT);
	GU_NOTE("FWSEC mapper: version=%u size=%u cmd_offset=%#x cmd_size=%u\n",
		r16(mapper+4), r16(mapper+6), r32(mapper+8), r32(mapper+12));
	/* Only the audited V3 mapper layout is supported. */
	if (r16(mapper + 4) != 3 || r16(mapper + 6) < 64)
		return GU_DIAG(-EOPNOTSUPP);
	cmd_off = r32(mapper + 8);
	cmd_size = r32(mapper + 12);
	GU_NOTE("FWSEC mapper offset=%#zx init_before=%#x cmd_off=%#x cmd_size=%#x\n",
		(size_t)(mapper - dmem), r32(mapper + 44), cmd_off, cmd_size);
	if (cmd_size < sizeof(cmd) || range(fw->dmem_size, cmd_off, cmd_size))
		return GU_DIAG(-EINVAL);
	w32(cmd, 1);
	w32(cmd + 4, 24);
	w32(cmd + 20, 2);
	w32(cmd + 24, 1);
	w32(cmd + 28, 20);
	w32(cmd + 32, frts_offset >> 12);
	w32(cmd + 36, 0x100);
	w32(cmd + 40, 2);
	memcpy(dmem + cmd_off, cmd, sizeof(cmd));
	w32(mapper + 44, 0x15);
	GU_NOTE("FWSEC mapper patched init=%#x cmd0=%#x cmd4=%#x\n",
		r32(mapper + 44), r32(dmem + cmd_off), r32(dmem + cmd_off + 4));
	return 0;
}
