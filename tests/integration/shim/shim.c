// SPDX-License-Identifier: GPL-2.0-only
/* Userspace kernel shim: every operation is a plain C call into the device. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"

struct bar_window {
	void *base;
	struct gd_device *dev;
};

static struct bar_window windows[8];
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;

static struct bar_window *window_for(const void *address)
{
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(windows); i++) {
		struct bar_window *w = &windows[i];
		if (w->base && (const char *)address >= (const char *)w->base &&
		    (const char *)address < (const char *)w->base + GD_BAR_SIZE)
			return w;
	}
	return NULL;
}

static u32 pci_command(struct pci_dev *pdev)
{
	u32 command = 0;
	gd_pci_read(pdev->dev.gd, 0, PCI_COMMAND, 2, &command);
	return command;
}

static void pci_set_command(struct pci_dev *pdev, u32 command)
{
	gd_pci_write(pdev->dev.gd, 0, PCI_COMMAND, 2, command);
}

void *devres_open_group(struct device *dev, void *id, int flags)
{
	(void)flags;
	unsigned int *group;
	if (id)
		return NULL;
	group = malloc(sizeof(*group));
	if (!group)
		return NULL;
	*group = dev->devres_head.count;
	dev->devres_head.count++;
	return group;
}

int devres_release_group(struct device *dev, void *id)
{
	unsigned int *group = id;
	int released;
	if (!group)
		return 0;
	released = (int)dev->devres_head.count - (int)*group - 1;
	if (released < 0)
		released = 0;
	dev->devres_head.count = *group;
	free(group);
	return released;
}

ktime_t ktime_get(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ktime_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void usleep_range(unsigned long min, unsigned long max)
{
	struct timespec ts;
	(void)max;
	ts.tv_sec = min / 1000000;
	ts.tv_nsec = (min % 1000000) * 1000;
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

u32 readl(const void __iomem *address)
{
	struct bar_window *w = window_for(address);
	if (!w)
		return 0xffffffffu;
	return gd_bar0_read(w->dev, (u32)((const char *)address - (const char *)w->base));
}

void writel(u32 value, void __iomem *address)
{
	struct bar_window *w = window_for(address);
	if (!w)
		return;
	gd_bar0_write(w->dev, (u32)((char *)address - (char *)w->base), value);
}

int memcpy_fromio(void *dest, const void __iomem *source, size_t size)
{
	struct bar_window *w = window_for(source);
	if (!w)
		return -EINVAL;
	return gd_prom_copy(w->dev, (u32)((const char *)source - (const char *)w->base), dest, size);
}

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *address, int flags)
{
	static uint64_t next = 0x10000000;
	void *cpu;
	(void)flags;
	size = (size + 255) & ~(size_t)255;
	if (!size)
		size = 256;
	cpu = calloc(1, size);
	if (!cpu)
		return NULL;
	*address = next;
	next += (size + 0xfff) & ~0xfffull;
	if (gd_dma_map(dev->gd, *address, cpu, size)) {
		free(cpu);
		return NULL;
	}
	return cpu;
}

void dma_free_coherent(struct device *dev, size_t size, void *cpu, dma_addr_t address)
{
	(void)size;
	gd_dma_unmap(dev->gd, address);
	free(cpu);
}

int dma_set_mask(struct device *dev, u64 mask)
{
	if (mask < 0xffffffffull)
		return -EIO;
	*dev->dma_mask = mask;
	return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
	if (mask < 0xffffffffull)
		return -EIO;
	dev->coherent_dma_mask = mask;
	return 0;
}

void dma_wmb(void)
{
	__sync_synchronize();
}

void *kzalloc(size_t size, int flags)
{
	(void)flags;
	return calloc(1, size ? size : 1);
}

int request_firmware(const struct firmware **out, const char *name, struct device *dev)
{
	char path[4096];
	const char *base = strrchr(name, '/');
	struct firmware *fw;
	FILE *file;
	long size;

	base = base ? base + 1 : name;
	snprintf(path, sizeof(path), "%s/%s", gd_asset_root(dev->gd), base);
	file = fopen(path, "rb");
	if (!file)
		return -ENOENT;
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) {
		fclose(file);
		return -EIO;
	}
	fw = malloc(sizeof(*fw));
	if (!fw) {
		fclose(file);
		return -ENOMEM;
	}
	fw->size = (size_t)size;
	fw->data = malloc(fw->size);
	if (!fw->data) {
		free(fw);
		fclose(file);
		return -ENOMEM;
	}
	if (fread((void *)fw->data, 1, fw->size, file) != fw->size) {
		free((void *)fw->data);
		free(fw);
		fclose(file);
		return -EIO;
	}
	fclose(file);
	*out = fw;
	return 0;
}

void release_firmware(const struct firmware *fw)
{
	if (!fw)
		return;
	free((void *)fw->data);
	free((void *)fw);
}

int pci_read_config_word(struct pci_dev *pdev, int offset, u16 *value)
{
	u32 v;
	int ret = gd_pci_read(pdev->dev.gd, 0, offset, 2, &v);
	*value = (u16)v;
	return ret;
}

int pci_read_config_dword(struct pci_dev *pdev, int offset, u32 *value)
{
	return gd_pci_read(pdev->dev.gd, 0, offset, 4, value);
}

int pci_write_config_word(struct pci_dev *pdev, int offset, u16 value)
{
	return gd_pci_write(pdev->dev.gd, 0, offset, 2, value);
}

int pcie_capability_read_word(struct pci_dev *pdev, int offset, u16 *value)
{
	u32 v;
	int ret = gd_pci_read(pdev->dev.gd, 1, offset, 2, &v);
	*value = (u16)v;
	return ret;
}

int pcie_capability_read_dword(struct pci_dev *pdev, int offset, u32 *value)
{
	return gd_pci_read(pdev->dev.gd, 1, offset, 4, value);
}

int pci_is_pcie(struct pci_dev *pdev)
{
	(void)pdev;
	return 1;
}

int pci_select_bars(struct pci_dev *pdev, int flags)
{
	(void)pdev;
	(void)flags;
	return 1;
}

int pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	(void)pdev;
	(void)bar;
	(void)name;
	return 0;
}

void pci_release_region(struct pci_dev *pdev, int bar)
{
	(void)pdev;
	(void)bar;
}

int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev, const char *name)
{
	(void)pdev;
	(void)name;
	return 0;
}

int pci_enable_device_mem(struct pci_dev *pdev)
{
	pci_set_command(pdev, pci_command(pdev) | PCI_COMMAND_MEMORY);
	return 0;
}

void pci_disable_device(struct pci_dev *pdev)
{
	pci_set_command(pdev, pci_command(pdev) & ~PCI_COMMAND_MEMORY);
}

resource_size_t pci_resource_len(struct pci_dev *pdev, int bar)
{
	(void)pdev;
	(void)bar;
	return GD_BAR_SIZE;
}

void *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	unsigned int i;
	void *base;
	(void)bar;
	(void)maxlen;
	base = gd_bar_pointer(pdev->dev.gd);
	pthread_mutex_lock(&window_lock);
	for (i = 0; i < ARRAY_SIZE(windows); i++) {
		if (!windows[i].base) {
			windows[i].base = base;
			windows[i].dev = pdev->dev.gd;
			pthread_mutex_unlock(&window_lock);
			return base;
		}
	}
	pthread_mutex_unlock(&window_lock);
	return NULL;
}

void pci_iounmap(struct pci_dev *pdev, void *address)
{
	unsigned int i;
	(void)pdev;
	pthread_mutex_lock(&window_lock);
	for (i = 0; i < ARRAY_SIZE(windows); i++) {
		if (windows[i].base == address) {
			windows[i].base = NULL;
			windows[i].dev = NULL;
		}
	}
	pthread_mutex_unlock(&window_lock);
}

void *pci_map_rom(struct pci_dev *pdev, size_t *size)
{
	(void)pdev;
	if (size)
		*size = 0;
	return NULL;
}

void pci_unmap_rom(struct pci_dev *pdev, void *rom)
{
	(void)pdev;
	(void)rom;
}

int pci_save_state(struct pci_dev *pdev)
{
	pdev->saved_command = pci_command(pdev);
	return 0;
}

void pci_restore_state(struct pci_dev *pdev)
{
	pci_set_command(pdev, pdev->saved_command);
}

void pci_set_master(struct pci_dev *pdev)
{
	pci_set_command(pdev, pci_command(pdev) | PCI_COMMAND_MASTER);
}

void pci_clear_master(struct pci_dev *pdev)
{
	pci_set_command(pdev, pci_command(pdev) & ~PCI_COMMAND_MASTER);
}

int pcie_flr(struct pci_dev *pdev)
{
	return gd_reset(pdev->dev.gd);
}

struct pci_dev *pci_dev_get(struct pci_dev *pdev)
{
	if (pdev)
		__atomic_add_fetch(&pdev->refs, 1, __ATOMIC_SEQ_CST);
	return pdev;
}

void pci_dev_put(struct pci_dev *pdev)
{
	if (pdev)
		__atomic_sub_fetch(&pdev->refs, 1, __ATOMIC_SEQ_CST);
}
