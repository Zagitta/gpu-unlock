/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_PAYLOAD_H
#define GPU_UNLOCK_PAYLOAD_H

#include "prep_610.h"

/* One word baked into the signature image at a fixed DMEM-relative offset.
 * These are image offsets, not BAR0 registers. */
struct gu_payload_word {
	u32 offset, value;
};

/* What a completed SEC2 launch must show besides a halt and a reopened
 * RESET_PLM. Payloads whose exit returns the input metadata pointer in
 * MAILBOX0/1 use the echo variant; anything unmeasured stays on the generic
 * predicate rather than inventing a return convention. */
enum gu_payload_completion {
	GU_PAYLOAD_HALT_RESET_PLM,
	GU_PAYLOAD_HALT_RESET_PLM_ECHO,
};

/* One signed SEC2 Booter's DMEM program. Selected by the Booter, never by the
 * family: the gadget and frame offsets are relative to that image's layout, so
 * another Booter cannot use them. The address and value slots carry the single
 * privileged write of one invocation. */
struct gu_payload {
	const char *name;
	u32 size;		/* signature allocation in bytes */
	u32 fill;		/* body dword */
	u32 value_off, address_off;
	const struct gu_payload_word *words;
	size_t word_count;
	enum gu_payload_completion completion;
};

int gu_payload_check(const struct gu_payload *payload);
int gu_payload_build(const struct gu_payload *payload, u8 *image,
		     u32 address, u32 value);

#endif
