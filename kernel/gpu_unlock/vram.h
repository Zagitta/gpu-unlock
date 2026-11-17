/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_VRAM_H
#define GPU_UNLOCK_VRAM_H

/* Serve the VRAM temperatures of every supported card through hwmon, each
 * card once nvidia has started it. Call once no unlock is running. */
void gu_vram_publish(void);
void gu_vram_remove(void);

#endif
