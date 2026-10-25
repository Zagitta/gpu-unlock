/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_UNLOCK_610_H
#define GPU_UNLOCK_UNLOCK_610_H

#include <linux/types.h>

struct firmware;
struct pci_dev;

/* Read-only firmware every unlock uses: loaded once, shared by concurrent unlocks. */
struct gpu_unlock_610_shared {
	const struct firmware *gsp;
};

/*
 * Load the shared firmware. pdev is any target card; the request leaves no
 * device resources behind on it. Returns 0 or a negative errno.
 */
int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared);

/* Release the shared firmware once every unlock using it has returned. */
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared);

/*
 * Unlock one supported cold, unbound card, or with resume one that nvidia
 * suspended before a suspend to RAM and resumes after this returns.
 * Experimental AD107 is startup-only: resume returns -EOPNOTSUPP without
 * acquiring transaction resources. The caller holds
 * pci_dev_lock. Returns 0 when the card is unlocked and handed back as found;
 * otherwise a negative errno, with the card left locked. After resume,
 * -EALREADY means the card kept power and is still unlocked, and nothing was
 * written. Unlocks of different cards may run concurrently with the same
 * shared firmware.
 */
int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared,
			  bool resume);

#endif
