/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_PAYLOAD_H
#define GPU_UNLOCK_PAYLOAD_H

#include "prep.h"

/* One word baked into the signature image at a fixed DMEM-relative offset.
 * These are image offsets, not BAR0 registers. */
struct gu_payload_word {
	u32 offset, value;
};

/* One BAR0 write, shared by the host and Booter paths. The register becomes
 * (current & keep) | value, and check_mask selects the bits compared against
 * that resolved value. Optional failures are logged without stopping. */
struct gu_reg_write {
	u32 addr, value;
	u32 keep;
	u32 check_mask;
	bool optional;
};

/* Slots the recipe reserves for one launch. A payload that can perform
 * several writes per SEC2 start lists one pair per write, in issue order. */
struct gu_payload_slot {
	u32 value_off, address_off;
};

#define GU_PAYLOAD_MAX_WRITES 8

/* What a completed SEC2 launch must show besides a halt and a reopened
 * RESET_PLM. Payloads whose exit returns the input metadata pointer in
 * MAILBOX0/1 use the echo variant; payloads that deliberately leave a
 * completion marker in MAILBOX0 use the marker variant. Anything unmeasured
 * stays on the generic predicate rather than inventing a return convention. */
enum gu_payload_completion {
	GU_PAYLOAD_HALT_RESET_PLM,
	GU_PAYLOAD_HALT_RESET_PLM_ECHO,
	GU_PAYLOAD_HALT_RESET_PLM_MARKER,
	GU_PAYLOAD_HALT_RESET_PLM_ZERO, /* MAILBOX0 must be zero */
};

/* One signed SEC2 Booter's DMEM program. Selected by the Booter, never by the
 * family: the entry and frame offsets are relative to that image's layout, so
 * another Booter cannot use them. slots[] carries the writes a single launch
 * performs; the executor reserves the last slot for WPR2 restoration and
 * repeats the last configuration write to fill a partial batch. */
struct gu_payload {
	const char *name;
	u32 size;		/* signature allocation in bytes */
	const struct gu_payload_slot *slots;
	size_t writes;		/* slots[] entries, writes per launch */
	const struct gu_payload_word *words;
	size_t word_count;
	enum gu_payload_completion completion;
	u32 marker;		/* MAILBOX0 for GU_PAYLOAD_HALT_RESET_PLM_MARKER */
};

int gu_payload_check(const struct gu_payload *payload);
int gu_payload_build(const struct gu_payload *payload, u8 *image,
		     const struct gu_reg_write *writes, size_t count);

#endif
