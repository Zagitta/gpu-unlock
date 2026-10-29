/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_UNLOCK_610_H
#define GPU_UNLOCK_UNLOCK_610_H

#include "firmware_610.h"

/*
 * Run the selected family recipe on a cold, unbound supported card, or one nvidia suspended
 * before a suspend to RAM and resumes after this returns. The caller holds
 * pci_dev_lock and supplies the context resolved during enumeration. Returns
 * 0 when the card is unlocked and handed back as found;
 * otherwise a negative errno. A failed transaction may have applied a prefix
 * of the table: it must not be reported as ready. If quiescence cannot be
 * established, DMA buffers and BAR ownership are retained until power off.
 * -EALREADY means the card is still unlocked from earlier this power cycle
 * (it kept power across a reboot or a sleep), and nothing was written.
 * direct lets a family whose Booter reaches the selectors write them in its
 * single launch without opening any PLM. Unlocks of different cards may run
 * concurrently with the same shared firmware.
 */
int gpu_unlock_610_unlock(const struct gu_device *device, const struct gpu_unlock_610_shared *shared,
			  bool resume, bool direct);

#endif
