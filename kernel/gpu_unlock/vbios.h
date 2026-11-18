/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_VBIOS_H
#define GPU_UNLOCK_VBIOS_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* Production FWSEC descriptor extracted from a VBIOS image. */
struct gpu_unlock_fwsec {
	u32 version;
	u32 image_off;
	u32 image_size;
	u32 imem_pa;
	u32 imem_size;
	u32 imem_va;
	u32 imem_ns_size, imem_sec_pa, imem_sec_size;
	u32 dmem_pa;
	u32 data_off;
	u32 dmem_size;
	u32 pkc_off;
	u32 interface_off;
	u32 ucode_id;
	u32 engine_mask;
	u32 sig_count;
	u32 sig_versions;
	u32 sig_off;
	u32 sig_bytes;
};

int gpu_unlock_vbios_fwsec(const u8 *rom, size_t length, struct gpu_unlock_fwsec *out);
/* Version-guarded single-byte SKU override, relative to the PCI image.
 * -EALREADY means the original VBIOS already advertises ECC. */
int gpu_unlock_vbios_ecc_override(const u8 *rom, size_t length, u8 record[11]);
/* Expand the TGP range in one record, preserving its factory default. Zero
 * keeps a bound; -EALREADY means the requested range is already available.
 * Update the working ROM and checksum so subsequent records compose with it. */
int gpu_unlock_vbios_power_overrides(u8 *rom, size_t length, u32 minimum_mw,
				    u32 maximum_mw, u8 record[22], u8 checksum[11]);
/* Enable +/- offset_mhz GPC offsets when the factory range is zero. Existing
 * ranges are preserved (-EALREADY). Update the working ROM and checksum. */
int gpu_unlock_vbios_gpc_offset_overrides(u8 *rom, size_t length, u16 offset_mhz,
					 u8 record[14], u8 checksum[11]);
/* Add GA100 P8 with the requested idle floors, plus its owning-image checksum.
 * Factory P0 and an existing P8 are preserved (-EALREADY). */
int gpu_unlock_vbios_p8_overrides(const u8 *rom, size_t length, u16 minimum_mhz,
				 u8 record[75], u8 checksum[11]);
int gpu_unlock_fwsec_patch_frts(u8 *image, const struct gpu_unlock_fwsec *fw,
				u64 frts_offset);
int gpu_unlock_fwsec_sig_offset(u32 versions, u32 fuse_version, u32 sig_size,
				u32 *offset);

#endif
