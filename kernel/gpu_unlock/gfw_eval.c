// SPDX-License-Identifier: GPL-2.0-only
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "gfw_eval.h"

/*
 * A powered-down engine reads all ones, and a Falcon PRI error reads back the
 * 0xbadfxxxx family. Both carry the bits the tests below look for, so treat
 * them as inaccessible here as the callers' own inaccessible() helper does.
 */
static bool inaccessible(u32 value)
{
	return value == (u32)~0U || (value & 0xffff0000u) == 0xbadf0000u;
}

int gpu_unlock_gfw_assess(const struct gpu_unlock_gfw_sample *sample)
{
	if (!sample || !sample->config_ok)
		return -ENODEV;
	if (inaccessible(sample->plm) || inaccessible(sample->boot) ||
	    inaccessible(sample->cpuctl))
		return -ENODEV;
	if (!(sample->cpuctl & GU_GFW_CPUCTL_HALTED))
		return -EAGAIN;
	if (!(sample->plm & GU_GFW_PLM_LEVEL0))
		return -EAGAIN;
	if ((sample->boot & 0xffu) != GU_GFW_BOOT_COMPLETED)
		return -EAGAIN;
	return 0;
}

u16 gpu_unlock_command_restore(u16 original, u16 live)
{
	u16 mask = GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER;

	return (live & (u16)~mask) | (original & mask);
}

bool gpu_unlock_pmcsr_d0(u16 pmcsr)
{
	return (pmcsr & GU_PCI_PM_CTRL_STATE_MASK) == GU_PCI_PM_STATE_D0;
}
