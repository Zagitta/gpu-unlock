/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_UNLOCK_H
#define GPU_UNLOCK_UNLOCK_H

#include "firmware.h"

enum gu_unlock_mode {
	GU_UNLOCK_BOOT,
	GU_UNLOCK_RESUME,
	GU_UNLOCK_RUNTIME_RESUME,
};

/*
 * Run the selected family recipe on a cold, unbound supported card, or one nvidia suspended
 * before system or runtime suspend and resumes after this returns. Boot and
 * system resume callers hold pci_dev_lock; runtime resume is serialized by
 * runtime PM instead, whose caller may already hold that lock. Returns
 * 0 when the card is unlocked and handed back as found;
 * otherwise a negative errno. A failed transaction may have applied a prefix
 * of the table: it must not be reported as ready. If quiescence cannot be
 * established, DMA buffers and BAR ownership are retained until power off.
 * Unlocks of different cards may run concurrently with the same
 * shared firmware.
 */
int gpu_unlock_run(const struct gu_device *device, const struct gpu_unlock_shared *shared,
			  enum gu_unlock_mode mode);

#endif
