// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "gfw_eval.h"

int main(void)
{
	struct gpu_unlock_gfw_sample sample = {
		.config_ok = true,
		.plm = GU_GFW_PLM_LEVEL0,
		.boot = GU_GFW_BOOT_COMPLETED,
		.cpuctl = GU_GFW_CPUCTL_HALTED,
	};

	assert(gpu_unlock_gfw_assess(&sample) == 0);
	sample.cpuctl = 0;
	assert(gpu_unlock_gfw_assess(&sample) == -EAGAIN);
	sample.cpuctl = GU_GFW_CPUCTL_HALTED;
	sample.plm = 0;
	assert(gpu_unlock_gfw_assess(&sample) == -EAGAIN);
	sample.plm = GU_GFW_PLM_LEVEL0;
	sample.boot = 0xfe;
	assert(gpu_unlock_gfw_assess(&sample) == -EAGAIN);
	sample.boot = (u32)~0U;
	sample.plm = (u32)~0U;
	sample.cpuctl = (u32)~0U;
	assert(gpu_unlock_gfw_assess(&sample) == -ENODEV);
	sample.boot = 0xbadf00ff;
	sample.plm = 0xbadf0001;
	sample.cpuctl = 0xbadf0010;
	assert(gpu_unlock_gfw_assess(&sample) == -EAGAIN);
	sample.boot = GU_GFW_BOOT_COMPLETED;
	sample.plm = GU_GFW_PLM_LEVEL0;
	sample.cpuctl = GU_GFW_CPUCTL_HALTED;
	sample.config_ok = false;
	assert(gpu_unlock_gfw_assess(&sample) == -ENODEV);

	assert(gpu_unlock_command_restore(0, GU_PCI_COMMAND_MEMORY |
					  GU_PCI_COMMAND_MASTER) == 0);
	assert(gpu_unlock_command_restore(GU_PCI_COMMAND_MEMORY,
					  GU_PCI_COMMAND_MEMORY |
					  GU_PCI_COMMAND_MASTER) ==
	       GU_PCI_COMMAND_MEMORY);
	assert(gpu_unlock_command_restore(GU_PCI_COMMAND_IO,
					  GU_PCI_COMMAND_IO | GU_PCI_COMMAND_MEMORY) ==
	       GU_PCI_COMMAND_IO);

	puts("gfw predicate and command restore checks passed");
	return 0;
}
