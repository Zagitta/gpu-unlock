/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_HBM_H
#define GPU_UNLOCK_HBM_H

#include <linux/io.h>

#define GU_HBM_POLL_MS 20
#define GU_HBM_IDLE_NS 100000000ull
#define GU_HBM_IDLE_NDIV 20u
#define GU_HBM_IDLE_PLDIV 1u
#define GU_HBM_IDLE_REFRESH 48u

enum gu_hbm_mode {
	GU_HBM_AUTO,
	GU_HBM_BUSY,
	GU_HBM_IDLE,
};

struct gu_hbm_policy {
	u32 ndiv, pldiv, refresh, idle_ms;
	enum gu_hbm_mode mode;
};

struct gu_hbm_counter {
	u32 mask[3], control, threshold, count, delta;
};

struct gu_hbm {
	struct gu_hbm_policy policy;
	struct gu_hbm_counter counter[16];
	u16 counters, active_counters;
	u32 fbpas, first_fbpa, busy_coeff, idle_coeff, busy_refresh[24];
	u64 sampled, quiet_since;
	bool profile, baseline, idle, fb_stopped;
	/* Last failed hardware check, for the worker's rate-limited log. */
	const char *reason;
	u32 reg, value, coverage[3];
};

/* Called with PCI access held. Only poll and restore write registers. */
bool gu_hbm_booted(void __iomem *bar);
void gu_hbm_init(struct gu_hbm *hbm);
int gu_hbm_poll(struct gu_hbm *hbm, void __iomem *bar, u64 now);
int gu_hbm_restore(struct gu_hbm *hbm, void __iomem *bar);
void gu_hbm_reset(struct gu_hbm *hbm);

struct pci_dev;
struct gu_hbm_worker;
struct gu_hbm_worker *gu_hbm_start(struct pci_dev *pdev);
void gu_hbm_remove(struct gu_hbm_worker *worker);

#endif
