/* SPDX-License-Identifier: GPL-2.0-only */
/* Userspace kernel API shim for the gpu_unlock driver. Every call below is a
 * plain C call into the device library; no Python and no mock driver. */
#ifndef GD_SHIM_COMPAT_H
#define GD_SHIM_COMPAT_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../device/gd.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
typedef int64_t ktime_t;

#define __iomem
#define __init
#define __exit
#define GFP_KERNEL 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((u64)(v) >> 32))
#define DMA_BIT_MASK(n) ((1ull << (n)) - 1)
#define MODULE_FIRMWARE(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_PARM_DESC(x, y)
#define THIS_MODULE NULL

/* dev_* receives a struct device *, which always belongs to a pci_dev. */
#define dev_err(dev, ...) gd_log((dev)->gd, GD_LOG_ERROR, __VA_ARGS__)
#define dev_info(dev, ...) gd_log((dev)->gd, GD_LOG_INFO, __VA_ARGS__)
#define dev_dbg(dev, ...) gd_log((dev)->gd, GD_LOG_DEBUG, __VA_ARGS__)
/* The firmware/driver diagnostics are useful when investigating a failured
 * run but make the acceptance output unreadable.  They are opt-in. */
static inline bool gd_diag_on(void)
{
	const char *on = getenv("GPU_UNLOCK_DIAG");
	return on && *on && strcmp(on, "0");
}
#define pr_info(...) do { if (gd_diag_on()) fprintf(stderr, __VA_ARGS__); } while (0)
#define pr_err(...) do { if (gd_diag_on()) fprintf(stderr, __VA_ARGS__); } while (0)
#define pr_debug(...) do { if (gd_diag_on()) fprintf(stderr, __VA_ARGS__); } while (0)

#define PCI_D0 0
#define PCI_UNKNOWN 5
#define PCI_DEV_FLAGS_NO_FLR_RESET (1u << 10)
#define PCI_VENDOR_ID 0x00
#define PCI_COMMAND 0x04
#define PCI_COMMAND_MEMORY 0x02
#define PCI_COMMAND_MASTER 0x04
#define PCI_PM_CTRL 0x04
#define PCI_PM_CTRL_STATE_MASK 0x03
#define PCI_EXP_DEVCAP 0x04
#define PCI_EXP_DEVCAP_FLR (1u << 28)
#define PCI_EXP_DEVSTA 0x0a
#define PCI_EXP_DEVSTA_TRPND 0x20
#define PCI_BASE_ADDRESS_0 0x10
#define IORESOURCE_MEM 0x200

struct list_head { unsigned int count; };

static inline bool list_empty(const struct list_head *h)
{
	return !h->count;
}

#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)

struct device {
	struct gd_device *gd;
	u64 *dma_mask;
	u64 coherent_dma_mask;
	struct list_head devres_head;
	int devres_lock;
};

struct pci_dev {
	struct device dev;
	unsigned short vendor, device;
	int current_state, pm_cap, refs;
	unsigned int dev_flags;
	unsigned int saved_command;
	unsigned int bars[6];
};

struct firmware {
	size_t size;
	const u8 *data;
};

void *devres_open_group(struct device *, void *, int);
static inline void __module_get(void *module) { (void)module; }
static inline void module_put(void *module) { (void)module; }
int devres_release_group(struct device *, void *);

void usleep_range(unsigned long min, unsigned long max);
ktime_t ktime_get(void);
static inline ktime_t ktime_add_us(ktime_t t, unsigned long us) { return t + (ktime_t)us; }
static inline int ktime_compare(ktime_t a, ktime_t b) { return a > b ? 1 : a < b ? -1 : 0; }

u32 readl(const void __iomem *address);
void writel(u32 value, void __iomem *address);
int memcpy_fromio(void *dest, const void __iomem *source, size_t size);

void *dma_alloc_coherent(struct device *, size_t, dma_addr_t *, int);
void dma_free_coherent(struct device *, size_t, void *, dma_addr_t);
int dma_set_mask(struct device *, u64);
int dma_set_coherent_mask(struct device *, u64);
void dma_wmb(void);

void *kzalloc(size_t, int);
static inline void *kvcalloc(size_t n, size_t s, int flags) { (void)flags; return calloc(n, s); }
static inline void *kvmalloc(size_t n, int flags) { (void)flags; return malloc(n); }
#define kfree free
#define kvfree free

int request_firmware(const struct firmware **, const char *, struct device *);
void release_firmware(const struct firmware *);

int pci_read_config_word(struct pci_dev *, int, u16 *);
int pci_read_config_dword(struct pci_dev *, int, u32 *);
int pci_write_config_word(struct pci_dev *, int, u16);
int pcie_capability_read_word(struct pci_dev *, int, u16 *);
int pcie_capability_read_dword(struct pci_dev *, int, u32 *);
int pci_is_pcie(struct pci_dev *);
int pci_select_bars(struct pci_dev *, int);
int pci_request_region(struct pci_dev *, int, const char *);
void pci_release_region(struct pci_dev *, int);
int aperture_remove_conflicting_pci_devices(struct pci_dev *, const char *);
int pci_enable_device_mem(struct pci_dev *);
void pci_disable_device(struct pci_dev *);
resource_size_t pci_resource_len(struct pci_dev *, int);
void *pci_iomap(struct pci_dev *, int, unsigned long);
void pci_iounmap(struct pci_dev *, void *);
void *pci_map_rom(struct pci_dev *, size_t *);
void pci_unmap_rom(struct pci_dev *, void *);
int pci_save_state(struct pci_dev *);
void pci_restore_state(struct pci_dev *);
void pci_set_master(struct pci_dev *);
void pci_clear_master(struct pci_dev *);
int pcie_flr(struct pci_dev *);
struct pci_dev *pci_dev_get(struct pci_dev *);
void pci_dev_put(struct pci_dev *);

#endif
