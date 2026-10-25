/* SPDX-License-Identifier: GPL-2.0-only */
/* Just enough kernel for main.c. Async work runs on real threads, so the
 * test sees true concurrency and main.c's own synchronization. */
#ifndef MAIN_SHIM_H
#define MAIN_SHIM_H
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define __init
#define __exit
/* Retain registered bool parameters so tests can exercise opt-in behavior
 * without referring to a production variable that does not exist yet. */
struct test_bool_parameter {
	const char *name;
	bool *value;
	struct test_bool_parameter *next;
};
static struct test_bool_parameter *test_bool_parameters;
#define test_module_param_bool(param)						\
	static struct test_bool_parameter test_parameter_##param = { #param, &param, NULL }; \
	static void __attribute__((constructor)) test_register_##param(void)	\
	{										\
		test_parameter_##param.next = test_bool_parameters;			\
		test_bool_parameters = &test_parameter_##param;				\
	}
#define test_module_param_charp(param)
#define module_param(name, type, perm) test_module_param_##type(name)
#define MODULE_PARM_DESC(name, desc)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
/* Keep the entry points reachable so the test calls what the kernel would. */
#define module_init(fn) int (*test_module_init)(void) = fn;
#define module_exit(fn) void (*test_module_exit)(void) = fn;

#define PCI_VENDOR_ID_NVIDIA 0x10de
#define PCI_ANY_ID (~0u)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct pci_driver {
	const char *name;
};
struct pci_dev {
	const char *name;
	unsigned short vendor, device;
	unsigned short subsystem_vendor, subsystem_device;
	unsigned char revision;
	struct pci_driver *driver;
	int refs;
	pthread_mutex_t lock;
	pthread_t owner;
	bool locked;
};
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from);
struct pci_dev *pci_dev_get(struct pci_dev *);
void pci_dev_put(struct pci_dev *);
void pci_dev_lock(struct pci_dev *);
void pci_dev_unlock(struct pci_dev *);
static inline const char *pci_name(const struct pci_dev *pdev) { return pdev->name; }

void test_log(const struct pci_dev *, const char *, ...) __attribute__((format(printf, 2, 3)));
#define pci_err(pdev, ...) test_log(pdev, __VA_ARGS__)
#define pci_info(pdev, ...) test_log(pdev, __VA_ARGS__)
#define pr_err(...) test_log(NULL, __VA_ARGS__)

typedef uint64_t async_cookie_t;
typedef void (*async_func_t)(void *data, async_cookie_t cookie);
struct async_domain {
	pthread_mutex_t lock;
	pthread_t threads[16];
	unsigned int count;
};
#define ASYNC_DOMAIN_EXCLUSIVE(name) struct async_domain name = { .lock = PTHREAD_MUTEX_INITIALIZER }
async_cookie_t async_schedule_domain(async_func_t, void *, struct async_domain *);
void async_synchronize_full_domain(struct async_domain *);
#define GFP_KERNEL 0
static inline void *kzalloc(size_t size, int flags) { return calloc(1, size); }
#define kfree free

/* The list operations main.c uses. */
struct list_head {
	struct list_head *next, *prev;
};
#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }
static inline bool list_empty(const struct list_head *head) { return head->next == head; }
static inline void list_add_tail(struct list_head *node, struct list_head *head)
{
	node->prev = head->prev;
	node->next = head;
	head->prev->next = node;
	head->prev = node;
}
static inline void list_del(struct list_head *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
	node->next = node->prev = NULL;
}
#define list_entry(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define list_for_each_entry(pos, head, member)						\
	for (pos = list_entry((head)->next, __typeof__(*pos), member);			\
	     &pos->member != (head);							\
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member)					\
	for (pos = list_entry((head)->next, __typeof__(*pos), member),			\
	     n = list_entry(pos->member.next, __typeof__(*pos), member);		\
	     &pos->member != (head);							\
	     pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

#define PM_SUSPEND_PREPARE 0x0003
#define PM_POST_SUSPEND 0x0004
#define NOTIFY_DONE 0x0000
struct notifier_block {
	int (*notifier_call)(struct notifier_block *, unsigned long, void *);
	int priority;
};
int register_pm_notifier(struct notifier_block *);
int unregister_pm_notifier(struct notifier_block *);
#endif
