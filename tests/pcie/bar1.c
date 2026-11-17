// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <linux/pci_regs.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Only PCI and locking boundaries are simulated. The resize implementation
 * is taken directly from pcie.c by bar1.sh. */
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t resource_size_t;
#define __iomem
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define IORESOURCE_MEM 0x200
#define IORESOURCE_PREFETCH 0x2000
#define IORESOURCE_MEM_64 0x100000
#define IORESOURCE_DISABLED 0x10000000
#define IORESOURCE_UNSET 0x20000000
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define pci_info(pdev, ...) ((void)(pdev))
#define pci_err(pdev, ...) ((void)(pdev))

struct resource {
	u64 start, end;
	unsigned long flags;
	struct resource *parent, *sibling;
};
struct pci_host_bridge { bool preserve_config; };
struct pci_dev {
	struct resource resource[6];
	struct pci_host_bridge bridge;
	struct pci_dev *bus;
	int dev, capacity, calls, fail_error;
	bool clear_failure, clear_assignment, fail_claim, stuck_decoder;
	u64 sizes;
	u16 command;
	u32 config[128], saved_config_space[16];
};
struct gu_family { struct { unsigned int count; } bar1; };
static const struct gu_family family = { .bar1.count = 2 };
static struct resource root;
static _Thread_local bool locked;
static unsigned int host_writes;
static const u64 host_offset = 1ULL << 44;

static void mutex_lock(pthread_mutex_t *mutex)
{
	assert(!locked);
	assert(!pthread_mutex_lock(mutex));
	locked = true;
}
static void mutex_unlock(pthread_mutex_t *mutex)
{
	assert(locked);
	locked = false;
	assert(!pthread_mutex_unlock(mutex));
}
static u64 pci_resource_len(struct pci_dev *pdev, unsigned int bar)
{
	struct resource *r = &pdev->resource[bar];
	return r->start || r->end ? r->end - r->start + 1 : 0;
}
static int fls64(u64 value) { return value ? 64 - __builtin_clzll(value) : 0; }
static int pci_rebar_bytes_to_size(u64 bytes) { assert(bytes); return fls64(bytes) - 21; }
static u64 pci_rebar_size_to_bytes(int size) { return 1ULL << (size + 20); }
static int pcibios_err_to_errno(int error) { return error > 0 ? -EIO : error; }
static struct pci_host_bridge *pci_find_host_bridge(struct pci_dev *bus) { assert(locked); return &bus->bridge; }
static int pci_find_ext_capability(struct pci_dev *pdev, int cap) { (void)pdev; assert(locked && cap == PCI_EXT_CAP_ID_REBAR); return 0x100; }
static u64 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar) { assert(locked && bar == 1); return pdev->sizes; }
static int pci_read_config_word(struct pci_dev *pdev, int offset, u16 *value) { assert(locked && offset == PCI_COMMAND); *value = pdev->command; return 0; }
static int pci_write_config_word(struct pci_dev *pdev, int offset, u16 value) { assert(locked && offset == PCI_COMMAND); pdev->command = value; return 0; }
static int pci_read_config_dword(struct pci_dev *pdev, int offset, u32 *value) { assert(locked); *value = pdev->config[offset / 4]; return 0; }
static int pci_write_config_dword(struct pci_dev *pdev, int offset, u32 value)
{
	assert(locked && !(pdev->command & PCI_COMMAND_MEMORY));
	if (offset != 0x110 || !pdev->stuck_decoder)
		pdev->config[offset / 4] = value;
	return 0;
}
static u64 pci_bus_address(struct pci_dev *pdev, unsigned int bar) { return pdev->resource[bar].start - host_offset; }
static int gu_host_writes(void *dev, void *bar, resource_size_t len, const void *table, const char *stage)
{
	struct pci_dev *pdev = (struct pci_dev *)((char *)dev - __builtin_offsetof(struct pci_dev, dev));
	(void)bar; (void)len; (void)table; (void)stage;
	assert(locked && (pdev->command & PCI_COMMAND_MEMORY));
	pdev->config[0x110 / 4] = 1 | (16 << PCI_REBAR_CTRL_BAR_SHIFT);
	/* Changing the decoder can invalidate address bits in the old BAR. */
	pdev->config[PCI_BASE_ADDRESS_1 / 4] = 0xc;
	pdev->config[(PCI_BASE_ADDRESS_1 / 4) + 1] &= ~0xf;
	host_writes++;
	usleep(1000); /* Give the other resize threads time to contend. */
	return 0;
}
static void pci_release_resource(struct pci_dev *pdev, unsigned int bar)
{
	struct resource *r = &pdev->resource[bar];
	u64 size = pci_resource_len(pdev, bar);
	assert(locked && !(pdev->command & PCI_COMMAND_MEMORY));
	if (!r->parent)
		return;
	r->parent = r->sibling = NULL;
	r->start = 0;
	r->end = size - 1;
	r->flags |= IORESOURCE_UNSET;
}
static int pci_claim_resource(struct pci_dev *pdev, unsigned int bar)
{
	struct resource *r = &pdev->resource[bar];
	assert(locked && !r->parent && !r->sibling && r->start);
	if (pdev->fail_claim && pdev->calls)
		return -EBUSY;
	r->parent = &root;
	return 0;
}
static void assign_bars(struct pci_dev *pdev)
{
	for (unsigned int bar = 1; bar <= 3; bar += 2) {
		struct resource *r = &pdev->resource[bar];
		u64 size = pci_resource_len(pdev, bar);
		if (!r->flags || r->parent)
			continue;
		r->start = host_offset + ((u64)(bar + 1) << 36);
		r->end = r->start + size - 1;
		r->flags &= ~IORESOURCE_UNSET;
		r->parent = &root;
		u64 address = pci_bus_address(pdev, bar) | 0xc;
		for (unsigned int j = 0; j < 2; j++) {
			unsigned int reg = (PCI_BASE_ADDRESS_0 / 4) + bar + j;
			pdev->config[reg] = pdev->saved_config_space[reg] = address >> (j * 32);
		}
	}
}
static int pci_resize_resource(struct pci_dev *pdev, int bar, int size
#ifdef GU_RESIZE_EXCLUDE_BARS
			       , int exclude_bars
#endif
)
{
	int old = (pdev->config[0x110 / 4] & PCI_REBAR_CTRL_BAR_SIZE) >> PCI_REBAR_CTRL_BAR_SHIFT;
	assert(locked && bar == 1 && !(pdev->command & PCI_COMMAND_MEMORY));
	assert(old == 6); /* Every failed attempt must restore the old decoder. */
	assert(!pdev->resource[1].parent && !pdev->resource[3].parent);
#ifdef GU_RESIZE_EXCLUDE_BARS
	assert(exclude_bars == BIT(0));
#else
	assert(pdev->resource[1].flags & IORESOURCE_UNSET);
	assert(pdev->resource[3].flags & IORESOURCE_UNSET);
#endif
	pdev->calls++;
	pdev->config[0x110 / 4] = 1 | (size << PCI_REBAR_CTRL_BAR_SHIFT);
	if (size > pdev->capacity || pdev->fail_error) {
		pdev->config[0x110 / 4] = 1 | (old << PCI_REBAR_CTRL_BAR_SHIFT);
		if (pdev->clear_failure)
			pdev->resource[1] = pdev->resource[3] = (struct resource){0};
		return pdev->fail_error ? pdev->fail_error : -ENOSPC;
	}
	pdev->resource[1].end = pci_rebar_size_to_bytes(size) - 1;
	assign_bars(pdev);
	return 0;
}
static void pci_assign_unassigned_bus_resources(struct pci_dev *bus)
{
	assert(locked);
	assign_bars(bus);
	if (bus->clear_assignment) {
		/* A successful resize call does not excuse a missing BAR3. */
		bus->resource[3] = (struct resource){0};
	}
}

#include "bar1-code.c"

static void init_card(struct pci_dev *pdev)
{
	memset(pdev, 0, sizeof(*pdev));
	pdev->bus = pdev;
	pdev->capacity = 16;
	pdev->sizes = BIT_ULL(6) | BIT_ULL(14) | BIT_ULL(15) | BIT_ULL(16);
	pdev->command = PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE;
	/* BAR1 is the second ReBAR entry; don't assume the first entry. */
	pdev->config[0x108 / 4] = 2 << PCI_REBAR_CTRL_NBAR_SHIFT;
	pdev->config[0x110 / 4] = 1 | (6 << PCI_REBAR_CTRL_BAR_SHIFT);
	for (unsigned int bar = 1; bar <= 3; bar += 2) {
		struct resource *r = &pdev->resource[bar];
		r->start = host_offset + (1ULL << 32) + (bar == 3 ? 1ULL << 26 : 0);
		r->end = r->start + (bar == 1 ? 1ULL << 26 : 1ULL << 25) - 1;
		r->flags = IORESOURCE_MEM | IORESOURCE_MEM_64 | IORESOURCE_PREFETCH | 0xc;
		r->parent = r->sibling = &root;
		u64 address = pci_bus_address(pdev, bar) | 0xc;
		for (unsigned int j = 0; j < 2; j++) {
			unsigned int reg = (PCI_BASE_ADDRESS_0 / 4) + bar + j;
			pdev->config[reg] = pdev->saved_config_space[reg] = address >> (j * 32);
		}
	}
}
static void check_card(struct pci_dev *pdev, int size)
{
	assert(!locked && bars_assigned(pdev));
	assert(pci_resource_len(pdev, 1) == pci_rebar_size_to_bytes(size));
	assert(pci_resource_len(pdev, 3) == 1ULL << 25);
	assert((pdev->config[0x110 / 4] & PCI_REBAR_CTRL_BAR_SIZE) == (u32)size << PCI_REBAR_CTRL_BAR_SHIFT);
	assert(pdev->command == (PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE));
	for (unsigned int bar = 1; bar <= 3; bar += 2) {
		u64 address = pci_bus_address(pdev, bar) | 0xc;
		for (unsigned int j = 0; j < 2; j++) {
			unsigned int reg = (PCI_BASE_ADDRESS_0 / 4) + bar + j;
			assert(pdev->config[reg] == (u32)(address >> (j * 32)));
			assert(pdev->saved_config_space[reg] == pdev->config[reg]);
		}
	}
}
static void *resize_thread(void *arg)
{
	assert(!gu_pcie_bar1_resize(arg, NULL, 0, &family));
	return NULL;
}
int main(void)
{
	struct pci_dev pdev, original;
	for (int capacity = 6; capacity <= 16; capacity++) {
		init_card(&pdev);
		original = pdev;
		pdev.capacity = capacity;
		pdev.clear_failure = true;
		assert(!gu_pcie_bar1_resize(&pdev, NULL, 0, &family));
		check_card(&pdev, capacity < 14 ? 6 : capacity);
		if (capacity < 14) {
			assert(pdev.resource[1].start == original.resource[1].start);
			assert(pdev.resource[3].start == original.resource[3].start);
		}
	}
	init_card(&pdev);
	pdev.sizes = BIT_ULL(6);
	assert(!gu_pcie_bar1_resize(&pdev, NULL, 0, &family));
	check_card(&pdev, 6);
	init_card(&pdev);
	pdev.capacity = 14;
	assert(!gu_pcie_bar1_resize(&pdev, NULL, 0, &family));
	check_card(&pdev, 14);
	init_card(&pdev);
	pdev.clear_assignment = true;
	assert(!gu_pcie_bar1_resize(&pdev, NULL, 0, &family));
	check_card(&pdev, 6);
	init_card(&pdev);
	pdev.fail_error = -ENOMEM;
	assert(gu_pcie_bar1_resize(&pdev, NULL, 0, &family) == -ENOMEM);
	assert(pdev.calls == 1);
	check_card(&pdev, 6);
	init_card(&pdev);
	pdev.capacity = 6;
	pdev.fail_claim = true;
	assert(gu_pcie_bar1_resize(&pdev, NULL, 0, &family) == -EBUSY);
	assert(pdev.calls == 1 && !(pdev.command & PCI_COMMAND_MEMORY) && !locked);
	init_card(&pdev);
	pdev.stuck_decoder = true;
	assert(gu_pcie_bar1_resize(&pdev, NULL, 0, &family) == -EIO);
	assert(!(pdev.command & PCI_COMMAND_MEMORY) && !locked);
	init_card(&pdev);
	pdev.resource[1] = (struct resource){0};
	assert(gu_pcie_bar1_resize(&pdev, NULL, 0, &family) == -ENODEV);
	assert(!pdev.calls && !locked);
	init_card(&pdev);
	pdev.bridge.preserve_config = true;
	assert(!gu_pcie_bar1_resize(&pdev, NULL, 0, &family));
	check_card(&pdev, 6);
	struct pci_dev cards[8];
	pthread_t threads[ARRAY_SIZE(cards)];
	unsigned int before = host_writes;
	for (unsigned int i = 0; i < ARRAY_SIZE(cards); i++) {
		init_card(&cards[i]);
		cards[i].capacity = 14;
		cards[i].clear_failure = true;
		assert(!pthread_create(&threads[i], NULL, resize_thread, &cards[i]));
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(cards); i++) {
		assert(!pthread_join(threads[i], NULL));
		check_card(&cards[i], 14);
	}
	assert(host_writes == before + ARRAY_SIZE(cards));
	puts("BAR1 resize: allocation failures, original-window fallback, decoder and address recovery, and eight concurrent callers passed");
}
