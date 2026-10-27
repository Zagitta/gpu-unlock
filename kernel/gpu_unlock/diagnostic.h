/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_DIAGNOSTIC_H
#define GPU_UNLOCK_DIAGNOSTIC_H
#ifdef __KERNEL__
#include <linux/printk.h>
#define GU_NOTE(fmt, ...) pr_debug("gpu_unlock: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Userspace builds are the integration harness; keep diagnostics opt-in so
 * the acceptance output is readable.  The kernel build is unchanged. */
static inline int gu_diag_enabled(void)
{
    const char *on = getenv("GPU_UNLOCK_DIAG");
    return on && *on && strcmp(on, "0");
}
#define GU_NOTE(fmt, ...) do { \
    if (gu_diag_enabled()) fprintf(stderr, "gpu_unlock: " fmt, ##__VA_ARGS__); \
} while (0)
#endif

/* No extra device reads. Preserve the errno and identify the exact guard in
 * the matching source bundled with each kit, including pure parser failures. */
static inline int gu_diagnostic_result(int error, const char *file,
                                      const char *function, unsigned int line)
{
    if (error)
        GU_NOTE("failure %s:%u %s error=%d\n", file, line, function, error);
    return error;
}
#define GU_DIAG(result) gu_diagnostic_result((result), __FILE__, __func__, __LINE__)
#endif
