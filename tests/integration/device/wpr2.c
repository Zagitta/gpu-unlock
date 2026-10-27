// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stddef.h>

#include "wpr2.h"

void gd_wpr2_decode(uint32_t lo, uint32_t hi, struct gd_wpr2_range *out)
{
	out->start = (uint64_t)(lo & 0xfffffff0u) << 8;
	out->end = ((uint64_t)(hi & 0xfffffff0u) << 8) + GD_WPR2_ALIGN;
}

int gd_wpr2_encode(uint64_t start, uint64_t end, uint32_t *lo, uint32_t *hi)
{
	if (!lo || !hi || start >= end || (start & (GD_WPR2_ALIGN - 1)) ||
	    (end & (GD_WPR2_ALIGN - 1)) ||
	    (end < GD_WPR2_ALIGN) || ((end - GD_WPR2_ALIGN) >> 12) > 0x0fffffffu ||
	    (start >> 12) > 0x0fffffffu)
		return -EINVAL;
	*lo = (uint32_t)((start >> 12) << 4);
	*hi = (uint32_t)(((end - GD_WPR2_ALIGN) >> 12) << 4);
	return 0;
}

int gd_wpr2_fwsec_hi(uint32_t lo, uint32_t *hi)
{
	uint64_t start = (uint64_t)(lo & 0xfffffff0u) << 8;
	uint32_t dummy;

	if (!hi)
		return -EINVAL;
	/* The debug FWSEC image writes the inclusive 4 KiB end.  The Booter's
	 * decode adds the 128 KiB alignment term, so the conforming HI is the
	 * last aligned block of a 1 MiB FRTS window. */
	return gd_wpr2_encode(start, start + GD_WPR2_FRTS_SIZE, &dummy, hi);
}

int gd_wpr2_validate(const struct gd_wpr2_meta *m)
{
	uint64_t gsp_end, boot_end;

	if (!m || m->wpr_start >= m->wpr_end ||
	    (m->wpr_start & (GD_WPR2_ALIGN - 1)) ||
	    (m->wpr_end & (GD_WPR2_ALIGN - 1)))
		return -EINVAL;
	if (!m->frts_size || m->frts_offset >= m->wpr_end ||
	    (m->frts_offset & 0xfff))
		return -EINVAL;
	if (m->frts_offset != m->wpr_end - m->frts_size)
		return -EINVAL;
	if (m->gsp_offset <= m->wpr_start || m->gsp_offset >= m->wpr_end ||
	    m->gsp_size == 0)
		return -EINVAL;
	gsp_end = m->gsp_offset + m->gsp_size;
	if (gsp_end < m->gsp_offset || gsp_end > m->wpr_end)
		return -EINVAL;
	if (m->boot_offset <= m->wpr_start || m->boot_offset >= m->wpr_end ||
	    m->boot_size == 0)
		return -EINVAL;
	boot_end = m->boot_offset + m->boot_size;
	if (boot_end < m->boot_offset || boot_end > m->wpr_end)
		return -EINVAL;
	if (m->gsp_offset < m->boot_offset) {
		if (m->gsp_offset - m->wpr_start < m->meta_size)
			return -EINVAL;
		if (boot_end > m->frts_offset ||
		    m->boot_offset - m->gsp_offset < m->gsp_size)
			return -EINVAL;
	} else {
		if (m->boot_offset - m->wpr_start < m->meta_size)
			return -EINVAL;
		if (gsp_end > m->frts_offset ||
		    m->gsp_offset - m->boot_offset < m->boot_size)
			return -EINVAL;
	}
	return 0;
}

int gd_wpr2_selftest(void)
{
	struct gd_wpr2_range r;
	struct gd_wpr2_meta m;
	const uint64_t start = 0x5f5400000ull;
	const uint64_t end = 0x5fff00000ull;
	uint32_t lo, hi, norm;
	int ret = 0;

	if (gd_wpr2_encode(start, end, &lo, &hi) || lo != 0x5f54000u ||
	    hi != 0x5ffee00u)
		ret |= 1;
	gd_wpr2_decode(lo, hi, &r);
	if (r.start != start || r.end != end)
		ret |= 2;
	if (gd_wpr2_encode(0x5ffe00000ull, 0x5ffe00000ull + GD_WPR2_FRTS_SIZE,
			   &lo, &hi) || hi != 0x5ffee00u)
		ret |= 4;
	if (gd_wpr2_fwsec_hi(lo, &norm) || norm != hi)
		ret |= 8;
	/* Unaligned ranges are refused. */
	if (!gd_wpr2_encode(start + 0x1000, end, &lo, &hi))
		ret |= 16;
	if (!gd_wpr2_encode(start, end - 0x1000, &lo, &hi))
		ret |= 32;

	m = (struct gd_wpr2_meta){
		.wpr_start = 0x5f5400000ull, .wpr_end = 0x5fff00000ull,
		.frts_offset = 0x5ffe00000ull, .frts_size = 0x100000ull,
		.gsp_offset = 0x5f5400000ull + 0x100000ull,
		.gsp_size = 0x4000000ull,
		.boot_offset = 0x5fda00000ull, .boot_size = 0x6000ull,
		.meta_size = 0x100ull,
	};
	if (gd_wpr2_validate(&m))
		ret |= 64;
	m.frts_offset -= 0x1000; /* FRTS no longer at the end of WPR2 */
	if (!gd_wpr2_validate(&m))
		ret |= 128;
	m = (struct gd_wpr2_meta){
		.wpr_start = 0x5f5400000ull, .wpr_end = 0x5fff00000ull,
		.frts_offset = 0x5ffe00000ull, .frts_size = 0x100000ull,
		.gsp_offset = 0x5f5400000ull + 0x100000ull,
		.gsp_size = 0x4000000ull,
		.boot_offset = 0x5fda00000ull, .boot_size = 0x6000ull,
		.meta_size = 0x100ull,
	};
	m.boot_offset = m.gsp_offset + 0x100; /* GSP image overlaps the BL */
	if (!gd_wpr2_validate(&m))
		ret |= 256;
	return ret;
}
