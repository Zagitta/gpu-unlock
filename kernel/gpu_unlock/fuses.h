/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_FUSES_H
#define GPU_UNLOCK_FUSES_H

struct gu_device;

/* Record the card's fuse block after its boot unlock, which returned unlock.
 * The caller holds pci_dev_lock and no driver is bound. Cards may be captured
 * concurrently. */
void gu_fuses_capture(const struct gu_device *device, int unlock);
/* Serve every snapshot read-only from /sys/module/gpu_unlock/fuses/<bdf>.
 * Call once, after every capture has finished. */
void gu_fuses_publish(void);
void gu_fuses_remove(void);

#endif
