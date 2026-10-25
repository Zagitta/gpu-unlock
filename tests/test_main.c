// SPDX-License-Identifier: GPL-2.0-only
/* main.c against mocked PCI and a mocked unlock: which cards are unlocked,
 * that they run concurrently, that the shared firmware outlives them, and
 * that cards unlocked at boot are unlocked again after resume, before the
 * notifier chain reaches nvidia. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../kernel/gpu_unlock/main.c"

#define MAX_DEVICES 8
#define RTX_3090 0x2204
#define RTX_3070 0x2484
#define RTX_A2000 0x2531 /* GA106 professional: not supported */
#define RTX_4080_SUPER 0x2702 /* AD103 */
#define RTX_4060_TI 0x2803 /* AD106; same asset path as AD103 */

static struct pci_dev devices[MAX_DEVICES];
static unsigned int device_count;
static char gsp_marker;
#define fake_gsp ((const struct firmware *)&gsp_marker)
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cond = PTHREAD_COND_INITIALIZER;
static struct pci_driver nvidia_driver = { "nvidia" }, vfio_driver = { "vfio-pci" };
static struct {
	int loads, releases, fail_load;
	bool loaded;
	struct pci_dev *load_dev;
	int started, finished, in_flight, max_in_flight;
	int expected_concurrent; /* unlocks that must overlap */
	int unlocks[MAX_DEVICES], fail_unlock[MAX_DEVICES];
	int resumes[MAX_DEVICES], fail_resume[MAX_DEVICES];
	struct notifier_block *nb;
	int registrations;
	char log[16384];
} fake;

void test_log(const struct pci_dev *pdev, const char *format, ...)
{
	va_list args;
	size_t at;

	pthread_mutex_lock(&fake_lock);
	at = strlen(fake.log);
	if (pdev)
		at += snprintf(fake.log + at, sizeof(fake.log) - at, "%s: ", pdev->name);
	va_start(args, format);
	vsnprintf(fake.log + at, sizeof(fake.log) - at, format, args);
	va_end(args);
	pthread_mutex_unlock(&fake_lock);
}

struct pci_dev *pci_dev_get(struct pci_dev *pdev)
{
	__atomic_add_fetch(&pdev->refs, 1, __ATOMIC_SEQ_CST);
	return pdev;
}

void pci_dev_put(struct pci_dev *pdev)
{
	if (pdev)
		assert(__atomic_sub_fetch(&pdev->refs, 1, __ATOMIC_SEQ_CST) >= 1);
}

/* Like the kernel: returns the next match with a reference, drops from's. */
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from)
{
	unsigned int i = from ? (unsigned int)(from - devices) + 1 : 0;

	pci_dev_put(from);
	for (; i < device_count; i++)
		if (devices[i].vendor == vendor && (device == PCI_ANY_ID || devices[i].device == device))
			return pci_dev_get(&devices[i]);
	return NULL;
}

void pci_dev_lock(struct pci_dev *pdev)
{
	pthread_mutex_lock(&pdev->lock);
	pdev->owner = pthread_self();
	pdev->locked = true;
}

void pci_dev_unlock(struct pci_dev *pdev)
{
	assert(pdev->locked && pthread_equal(pdev->owner, pthread_self()));
	pdev->locked = false;
	pthread_mutex_unlock(&pdev->lock);
}

struct async_entry {
	async_func_t fn;
	void *data;
	async_cookie_t cookie;
};

static void *async_thread(void *arg)
{
	struct async_entry *entry = arg;

	entry->fn(entry->data, entry->cookie);
	free(entry);
	return NULL;
}

async_cookie_t async_schedule_domain(async_func_t fn, void *data, struct async_domain *domain)
{
	struct async_entry *entry = malloc(sizeof(*entry));
	async_cookie_t cookie;

	assert(entry);
	pthread_mutex_lock(&domain->lock);
	assert(domain->count < sizeof(domain->threads) / sizeof(domain->threads[0]));
	cookie = domain->count + 1;
	*entry = (struct async_entry){ fn, data, cookie };
	assert(!pthread_create(&domain->threads[domain->count++], NULL, async_thread, entry));
	pthread_mutex_unlock(&domain->lock);
	return cookie;
}

void async_synchronize_full_domain(struct async_domain *domain)
{
	pthread_mutex_lock(&domain->lock);
	for (unsigned int i = 0; i < domain->count; i++)
		assert(!pthread_join(domain->threads[i], NULL));
	domain->count = 0;
	pthread_mutex_unlock(&domain->lock);
}

int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared)
{
	pthread_mutex_lock(&fake_lock);
	/* Loaded once per round, before any unlock can use it. */
	assert(!fake.in_flight && !shared->gsp);
	fake.loads++;
	fake.load_dev = pdev;
	pthread_mutex_unlock(&fake_lock);
	if (fake.fail_load)
		return fake.fail_load;
	shared->gsp = fake_gsp;
	fake.loaded = true;
	return 0;
}

void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared)
{
	pthread_mutex_lock(&fake_lock);
	/* Only after every unlock has returned. */
	assert(!fake.in_flight && fake.finished == fake.started);
	assert(shared->gsp == (fake.loaded ? fake_gsp : NULL));
	fake.releases++;
	fake.loaded = false;
	shared->gsp = NULL;
	pthread_mutex_unlock(&fake_lock);
}

int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared,
			  bool resume)
{
	unsigned int index = pdev - devices;
	struct timespec deadline;
	int ret;

	assert(shared->gsp == fake_gsp);
	assert(pdev->locked && pthread_equal(pdev->owner, pthread_self()));
	/* Unbound at boot; after resume only a card nvidia suspended. */
	assert(resume ? pdev->driver == &nvidia_driver : !pdev->driver);
	/* The initial reference, the card list's, and nothing dropped early. */
	assert(__atomic_load_n(&pdev->refs, __ATOMIC_SEQ_CST) >= 2);
	pthread_mutex_lock(&fake_lock);
	if (resume)
		fake.resumes[index]++;
	else
		fake.unlocks[index]++;
	fake.started++;
	if (++fake.in_flight > fake.max_in_flight)
		fake.max_in_flight = fake.in_flight;
	pthread_cond_broadcast(&fake_cond);
	/* Wait until every expected card is inside an unlock at once. Serial
	 * unlocking never gets there and falls through on the timeout. */
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	while (fake.max_in_flight < fake.expected_concurrent &&
	       pthread_cond_timedwait(&fake_cond, &fake_lock, &deadline) != ETIMEDOUT)
		;
	pthread_mutex_unlock(&fake_lock);
	/* Still busy after the rendezvous, so an unsynchronized init would
	 * return, and release the firmware, while this card is mid-unlock. */
	usleep(20000);
	pthread_mutex_lock(&fake_lock);
	fake.in_flight--;
	fake.finished++;
	ret = resume ? fake.fail_resume[index] : fake.fail_unlock[index];
	pthread_mutex_unlock(&fake_lock);
	return ret;
}

int register_pm_notifier(struct notifier_block *nb)
{
	assert(!fake.nb);
	fake.nb = nb;
	fake.registrations++;
	return 0;
}

int unregister_pm_notifier(struct notifier_block *nb)
{
	assert(fake.nb == nb);
	fake.nb = NULL;
	return 0;
}

static void add(const char *name, unsigned short device, bool bound)
{
	struct pci_dev *pdev = &devices[device_count++];

	*pdev = (struct pci_dev){ .name = name, .vendor = PCI_VENDOR_ID_NVIDIA, .device = device,
				  .driver = bound ? &nvidia_driver : NULL, .refs = 1 };
	pthread_mutex_init(&pdev->lock, NULL);
}

static void reset(char *list)
{
	memset(&fake, 0, sizeof(fake));
	device_count = 0;
	targets = list;
	resume = true;
}

static void boot(void)
{
	assert(!test_module_init());
	assert(!fake.in_flight && fake.finished == fake.started);
	for (unsigned int i = 0; i < device_count; i++)
		assert(!devices[i].locked && fake.unlocks[i] <= 1);
}

/* Calls the notifier as the PM core does; returns once it has. */
static void notify(unsigned long event)
{
	assert(fake.nb && fake.nb->notifier_call(fake.nb, event, NULL) == NOTIFY_DONE);
	/* nvidia's notifier runs next: no unlock may still be running. */
	assert(!fake.in_flight && fake.finished == fake.started);
}

/* Unloads the module and checks every reference came back. */
static void shutdown(void)
{
	test_module_exit();
	assert(!fake.nb);
	for (unsigned int i = 0; i < device_count; i++)
		assert(devices[i].refs == 1 && !devices[i].locked);
}

/* Runs init, then checks what holds for every case. */
static void run(void)
{
	boot();
	shutdown();
	assert(fake.loads <= 1 && fake.releases <= 1);
}

static bool logged(const char *text)
{
	return strstr(fake.log, text);
}

int main(void)
{
	/* Every unbound supported card at once; other chips and bound cards left alone. */
	reset("all");
	add("0000:01:00.0", RTX_3090, false);
	add("0000:21:00.0", RTX_3070, false);
	add("0000:31:00.0", RTX_A2000, false);
	add("0000:41:00.0", RTX_3090, true);
	add("0000:61:00.0", 0x2208, false);
	add("0000:71:00.0", RTX_4080_SUPER, false);
	fake.expected_concurrent = 4;
	run();
	assert(fake.max_in_flight == 4 && fake.started == 4);
	assert(fake.unlocks[0] && fake.unlocks[1] && !fake.unlocks[2] && !fake.unlocks[3] && fake.unlocks[4] && fake.unlocks[5]);
	assert(fake.loads == 1 && fake.load_dev == &devices[0] && fake.releases == 1);
	assert(logged("0000:01:00.0: gpu_unlock: unlocked") && logged("0000:61:00.0: gpu_unlock: unlocked"));
	assert(logged("0000:71:00.0: gpu_unlock: unlocked"));
	assert(logged("0000:41:00.0: gpu_unlock: unlock failed (-16)"));
	puts("all: four cards unlocked concurrently, bound card skipped PASS");

	/* A list picks cards in any case; the rest are not touched. */
	reset("0000:61:00.0,0000:0A:00.0");
	add("0000:01:00.0", RTX_3090, false);
	add("0000:0a:00.0", RTX_3090, false);
	add("0000:61:00.0", RTX_3090, false);
	fake.expected_concurrent = 2;
	run();
	assert(fake.max_in_flight == 2 && !fake.unlocks[0] && fake.unlocks[1] && fake.unlocks[2]);
	assert(fake.loads == 1 && fake.load_dev == &devices[1] && fake.releases == 1);
	assert(!logged("no supported card"));
	puts("list: only listed cards, concurrently PASS");

	/* One card failing leaves the others unlocked. */
	reset("all");
	add("0000:01:00.0", RTX_3090, false);
	add("0000:21:00.0", RTX_3090, false);
	fake.fail_unlock[0] = -EIO;
	fake.expected_concurrent = 2;
	run();
	assert(fake.max_in_flight == 2);
	assert(logged("0000:01:00.0: gpu_unlock: unlock failed (-5)"));
	assert(logged("0000:21:00.0: gpu_unlock: unlocked"));
	puts("failure: other cards unaffected PASS");

	/* Without the shared firmware no card is attempted. */
	reset("all");
	add("0000:01:00.0", RTX_3090, false);
	add("0000:21:00.0", RTX_3090, false);
	fake.fail_load = -ENOENT;
	run();
	assert(fake.loads == 1 && !fake.started && fake.releases == 1 && !fake.registrations);
	assert(logged("0000:01:00.0: gpu_unlock: firmware load failed (-2)"));
	puts("firmware load failure: nothing attempted PASS");

	/* Unmatched entries, including unsupported chips, are reported; nothing to load for no cards. */
	reset("0000:99:00.0,0000:31:00.0");
	add("0000:01:00.0", RTX_3090, false);
	add("0000:31:00.0", RTX_A2000, false);
	run();
	assert(!fake.loads && !fake.started && fake.releases == 1 && !fake.registrations);
	assert(logged("gpu_unlock: no supported card at \"0000:99:00.0\""));
	assert(logged("gpu_unlock: no supported card at \"0000:31:00.0\""));
	puts("unmatched target: reported, nothing loaded PASS");

	/* No opt-in on the command line: the module does nothing. */
	for (int empty = 0; empty < 2; empty++) {
		reset(empty ? "" : NULL);
		add("0000:01:00.0", RTX_3090, false);
		run();
		assert(!fake.loads && !fake.started && !fake.releases && !fake.log[0]);
		assert(!fake.registrations);
	}
	puts("no targets: idle PASS");

	/* After resume, the cards unlocked at boot, concurrently, and all of them
	 * finished before the notifier returns. A card that failed at boot is not
	 * tried again; suspending does nothing. */
	reset("all");
	add("0000:01:00.0", RTX_3090, false);
	add("0000:21:00.0", RTX_3070, false);
	add("0000:41:00.0", RTX_3090, false);
	fake.fail_unlock[2] = -EIO;
	boot();
	assert(fake.registrations == 1 && fake.nb->priority > 0);
	for (unsigned int i = 0; i < device_count; i++)
		devices[i].driver = &nvidia_driver;
	notify(PM_SUSPEND_PREPARE);
	assert(fake.loads == 1 && fake.started == 3);
	fake.expected_concurrent = 2;
	fake.max_in_flight = 0;
	notify(PM_POST_SUSPEND);
	assert(fake.max_in_flight == 2 && fake.started == 5);
	assert(fake.resumes[0] == 1 && fake.resumes[1] == 1 && !fake.resumes[2]);
	assert(fake.loads == 2 && fake.load_dev == &devices[0] && fake.releases == 2);
	assert(logged("0000:01:00.0: gpu_unlock: unlocked again after resume"));
	assert(logged("0000:21:00.0: gpu_unlock: unlocked again after resume"));
	assert(!logged("0000:41:00.0: gpu_unlock: unlocked again"));
	shutdown();
	assert(fake.unlocks[0] == 1 && fake.unlocks[1] == 1 && fake.unlocks[2] == 1);
	puts("resume: boot-unlocked cards unlocked again before nvidia PASS");

	/* Only cards nvidia suspended are touched. A card that kept power, or one
	 * that fails, stays listed for the next resume. */
	reset("all");
	add("0000:01:00.0", RTX_3090, false);
	add("0000:21:00.0", RTX_3090, false);
	add("0000:31:00.0", RTX_3090, false);
	add("0000:61:00.0", RTX_3090, false);
	boot();
	devices[0].driver = &nvidia_driver;
	devices[1].driver = &nvidia_driver;
	devices[2].driver = &vfio_driver;
	fake.fail_resume[0] = -EALREADY;
	fake.fail_resume[1] = -EBUSY;
	notify(PM_POST_SUSPEND);
	assert(fake.resumes[0] == 1 && fake.resumes[1] == 1 && !fake.resumes[2] && !fake.resumes[3]);
	assert(logged("0000:01:00.0: gpu_unlock: still unlocked after resume"));
	assert(logged("0000:21:00.0: gpu_unlock: unlock after resume failed (-16)"));
	assert(logged("0000:31:00.0: gpu_unlock: not bound to nvidia, left as is after resume"));
	assert(logged("0000:61:00.0: gpu_unlock: not bound to nvidia, left as is after resume"));
	notify(PM_POST_SUSPEND);
	assert(fake.resumes[0] == 2 && fake.resumes[1] == 2 && !fake.resumes[2] && !fake.resumes[3]);
	shutdown();
	puts("resume: only nvidia-suspended cards, kept for the next resume PASS");

	/* gpu_unlock.resume=0, and a firmware load failure, leave the cards to nvidia. */
	reset("0000:01:00.0");
	add("0000:01:00.0", RTX_3090, false);
	boot();
	devices[0].driver = &nvidia_driver;
	resume = false;
	notify(PM_POST_SUSPEND);
	assert(fake.loads == 1 && !fake.resumes[0]);
	resume = true;
	fake.fail_load = -ENOENT;
	notify(PM_POST_SUSPEND);
	assert(fake.loads == 2 && fake.releases == 2 && !fake.resumes[0]);
	assert(logged("0000:01:00.0: gpu_unlock: firmware load failed (-2)"));
	fake.fail_load = 0;
	notify(PM_POST_SUSPEND);
	assert(fake.resumes[0] == 1);
	shutdown();
	puts("resume: switch off and firmware failure leave cards locked PASS");

	/* Nothing unlocked at boot: no notifier at all. */
	reset("all");
	add("0000:01:00.0", RTX_3090, false);
	fake.fail_unlock[0] = -EIO;
	boot();
	assert(!fake.registrations && !fake.nb);
	shutdown();
	puts("resume: no notifier without an unlocked card PASS");
	return 0;
}
