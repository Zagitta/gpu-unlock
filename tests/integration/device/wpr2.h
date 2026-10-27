/* SPDX-License-Identifier: GPL-2.0-only */
/* WPR2 register encoding and range rules.
 *
 * The values and tests follow booter_wpr2_tu10x.c and
 * booter_load_tu10x.c: the LO/HI fields are address >> 8, the end carries a
 * 128 KiB (0x20000) alignment term, and the FRTS window must be exactly the
 * 1 MiB the FWSEC command requests.  The debug FWSEC image publishes an
 * inclusive 4 KiB end; gd_wpr2_fwsec_hi() converts that convention for the
 * Booter, and the caller reports the conversion as evidence.
 */
#ifndef GD_WPR2_H
#define GD_WPR2_H

#include <stdbool.h>
#include <stdint.h>

#define GD_WPR2_ALIGN          0x20000u
#define GD_WPR2_FRTS_SIZE      0x100000u

struct gd_wpr2_range {
	uint64_t start;
	uint64_t end;          /* exclusive */
};

/* Booter view: start = LO << 12; end = (HI << 12) + 0x20000. */
void gd_wpr2_decode(uint32_t lo, uint32_t hi, struct gd_wpr2_range *out);

/* Booter view encoder.  end must be 128 KiB aligned. */
int gd_wpr2_encode(uint64_t start, uint64_t end, uint32_t *lo, uint32_t *hi);

/* FWSEC publishes the inclusive 4 KiB end as ((end - 1) >> 12) << 4.
 * Return the HI value the Booter expects for a 1 MiB FRTS window. */
int gd_wpr2_fwsec_hi(uint32_t lo, uint32_t *hi);

struct gd_wpr2_meta {
	uint64_t wpr_start, wpr_end;
	uint64_t frts_offset, frts_size;
	uint64_t gsp_offset, gsp_size;
	uint64_t boot_offset, boot_size;
	uint64_t meta_size;
};

/* Rules the Booter enforces on a WprMeta-proposed range. */
int gd_wpr2_validate(const struct gd_wpr2_meta *meta);

/* Unit-test entry: returns 0 when every vector passes. */
int gd_wpr2_selftest(void);

#endif
