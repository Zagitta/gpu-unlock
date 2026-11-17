/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_OVERRIDES_H
#define GPU_UNLOCK_OVERRIDES_H

struct gu_device;
/* Capture only from a cold, unlocked card while its PCI lock is held. */
int gu_overrides_capture(const struct gu_device *device);
int gu_overrides_publish(void);
void gu_overrides_remove(void);

#endif
