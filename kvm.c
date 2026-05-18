#include "kvm/kvm.h"
#include "kvm/read-write.h"
#include "kvm/util.h"
#include "kvm/strbuf.h"
#include "kvm/mutex.h"
#include "kvm/kvm-cpu.h"
#include "kvm/kvm-ipc.h"
#include "kvm/builtin-run.h"
#include "kvm/8250-serial.h"

/* Debug log file for child VM fork diagnostics */
FILE *hyperfork_debug_log = NULL;
int hyperfork_child_octet = 0;

void hyperfork_dbg(const char *fmt, ...)
{
	if (!hyperfork_debug_log)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(hyperfork_debug_log, "[%ld.%06ld] [pid=%d] ",
		ts.tv_sec, ts.tv_nsec / 1000, getpid());
	va_list args;
	va_start(args, fmt);
	vfprintf(hyperfork_debug_log, fmt, args);
	va_end(args);
	fprintf(hyperfork_debug_log, "\n");
	fflush(hyperfork_debug_log);
}

#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/list.h>
#include <linux/err.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <sys/eventfd.h>
#include <asm/unistd.h>
#include <dirent.h>

#define DEFINE_KVM_EXIT_REASON(reason) [reason] = #reason

const char *kvm_exit_reasons[] = {
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_UNKNOWN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_EXCEPTION),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_IO),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_HYPERCALL),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_DEBUG),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_HLT),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_MMIO),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_IRQ_WINDOW_OPEN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_SHUTDOWN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_FAIL_ENTRY),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_INTR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_SET_TPR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_TPR_ACCESS),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_S390_SIEIC),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_S390_RESET),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_DCR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_NMI),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_INTERNAL_ERROR),
#ifdef CONFIG_PPC64
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_PAPR_HCALL),
#endif
};

static int pause_event;
static DEFINE_MUTEX(pause_lock);
extern struct kvm_ext kvm_req_ext[];

static char kvm_dir[PATH_MAX];

extern __thread struct kvm_cpu *current_kvm_cpu;

u64 kvm__time_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		die("clock_gettime(CLOCK_MONOTONIC) failed");

	return (u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

double kvm__elapsed_ms(u64 start_ns)
{
	return (kvm__time_ns() - start_ns) / 1000000.0;
}

void kvm__fork_trace(struct pre_copy_context *ctxt, const char *fmt, ...)
{
	char msg[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	pr_info("kvm-fork[%d]: +%.3f ms %s",
		getpid(), kvm__elapsed_ms(ctxt->trace_start_ns), msg);
}

static void kvm__fork_trace_separator(struct pre_copy_context *ctxt,
				      const char *label)
{
	pr_info("kvm-fork[%d]: +%.3f ms ==================== %s ====================",
		getpid(), kvm__elapsed_ms(ctxt->trace_start_ns), label);
}

static int set_dir(const char *fmt, va_list args)
{
	char tmp[PATH_MAX];

	vsnprintf(tmp, sizeof(tmp), fmt, args);

	mkdir(tmp, 0777);

	if (!realpath(tmp, kvm_dir))
		return -errno;

	strcat(kvm_dir, "/");

	return 0;
}

void kvm__set_dir(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	set_dir(fmt, args);
	va_end(args);
}

const char *kvm__get_dir(void)
{
	return kvm_dir;
}

bool kvm__supports_vm_extension(struct kvm *kvm, unsigned int extension)
{
	static int supports_vm_ext_check = 0;
	int ret;

	switch (supports_vm_ext_check) {
	case 0:
		ret = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION,
			    KVM_CAP_CHECK_EXTENSION_VM);
		if (ret <= 0) {
			supports_vm_ext_check = -1;
			return false;
		}
		supports_vm_ext_check = 1;
		/* fall through */
	case 1:
		break;
	case -1:
		return false;
	}

	ret = ioctl(kvm->vm_fd, KVM_CHECK_EXTENSION, extension);
	if (ret < 0)
		return false;

	return ret;
}

bool kvm__supports_extension(struct kvm *kvm, unsigned int extension)
{
	int ret;

	ret = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, extension);
	if (ret < 0)
		return false;

	return ret;
}

static int kvm__check_extensions(struct kvm *kvm)
{
	int i;

	for (i = 0; ; i++) {
		if (!kvm_req_ext[i].name)
			break;
		if (!kvm__supports_extension(kvm, kvm_req_ext[i].code)) {
			pr_err("Unsupported KVM extension detected: %s",
				kvm_req_ext[i].name);
			return -i;
		}
	}

	return 0;
}

struct kvm *kvm__new(void)
{
	struct kvm *kvm = calloc(1, sizeof(*kvm));
	if (!kvm)
		return ERR_PTR(-ENOMEM);

	kvm->sys_fd = -1;
	kvm->vm_fd = -1;

	return kvm;
}

int kvm__exit(struct kvm *kvm)
{
	struct kvm_mem_bank *bank, *tmp;

	kvm__arch_delete_ram(kvm);

	list_for_each_entry_safe(bank, tmp, &kvm->mem_banks, list) {
		list_del(&bank->list);
		free(bank);
	}

	free(kvm);
	return 0;
}
core_exit(kvm__exit);

int kvm__register_mem(struct kvm *kvm, u64 guest_phys, u64 size,
		      void *userspace_addr, enum kvm_mem_type type)
{
	struct kvm_userspace_memory_region mem;
	struct kvm_mem_bank *merged = NULL;
	struct kvm_mem_bank *bank;
	int ret;

	/* Check for overlap */
	list_for_each_entry(bank, &kvm->mem_banks, list) {
		u64 bank_end = bank->guest_phys_addr + bank->size - 1;
		u64 end = guest_phys + size - 1;
		if (guest_phys > bank_end || end < bank->guest_phys_addr)
			continue;

		/* Merge overlapping reserved regions */
		if (bank->type == KVM_MEM_TYPE_RESERVED &&
		    type == KVM_MEM_TYPE_RESERVED) {
			bank->guest_phys_addr = min(bank->guest_phys_addr, guest_phys);
			bank->size = max(bank_end, end) - bank->guest_phys_addr + 1;

			if (merged) {
				/*
				 * This is at least the second merge, remove
				 * previous result.
				 */
				list_del(&merged->list);
				free(merged);
			}

			guest_phys = bank->guest_phys_addr;
			size = bank->size;
			merged = bank;

			/* Keep checking that we don't overlap another region */
			continue;
		}

		pr_err("%s region [%llx-%llx] would overlap %s region [%llx-%llx]",
		       kvm_mem_type_to_string(type), guest_phys, guest_phys + size - 1,
		       kvm_mem_type_to_string(bank->type), bank->guest_phys_addr,
		       bank->guest_phys_addr + bank->size - 1);

		return -EINVAL;
	}

	if (merged)
		return 0;

	bank = malloc(sizeof(*bank));
	if (!bank)
		return -ENOMEM;

	INIT_LIST_HEAD(&bank->list);
	bank->guest_phys_addr		= guest_phys;
	bank->host_addr			= userspace_addr;
	bank->size			= size;
	bank->type			= type;

	if (type != KVM_MEM_TYPE_RESERVED) {
		mem = (struct kvm_userspace_memory_region) {
			.slot			= kvm->mem_slots++,
			.guest_phys_addr	= guest_phys,
			.memory_size		= size,
			.userspace_addr		= (unsigned long)userspace_addr,
		};

		ret = ioctl(kvm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
		if (ret < 0)
			return -errno;
	}

	list_add(&bank->list, &kvm->mem_banks);

	return 0;
}

void *guest_flat_to_host(struct kvm *kvm, u64 offset)
{
	struct kvm_mem_bank *bank;

	list_for_each_entry(bank, &kvm->mem_banks, list) {
		u64 bank_start = bank->guest_phys_addr;
		u64 bank_end = bank_start + bank->size;

		if (offset >= bank_start && offset < bank_end)
			return bank->host_addr + (offset - bank_start);
	}

	pr_warning("unable to translate guest address 0x%llx to host",
			(unsigned long long)offset);
	return NULL;
}

u64 host_to_guest_flat(struct kvm *kvm, void *ptr)
{
	struct kvm_mem_bank *bank;

	list_for_each_entry(bank, &kvm->mem_banks, list) {
		void *bank_start = bank->host_addr;
		void *bank_end = bank_start + bank->size;

		if (ptr >= bank_start && ptr < bank_end)
			return bank->guest_phys_addr + (ptr - bank_start);
	}

	pr_warning("unable to translate host address %p to guest", ptr);
	return 0;
}

/*
 * Iterate over each registered memory bank. Call @fun for each bank with @data
 * as argument. @type is a bitmask that allows to filter banks according to
 * their type.
 *
 * If one call to @fun returns a non-zero value, stop iterating and return the
 * value. Otherwise, return zero.
 */
int kvm__for_each_mem_bank(struct kvm *kvm, enum kvm_mem_type type,
			   int (*fun)(struct kvm *kvm, struct kvm_mem_bank *bank, void *data),
			   void *data)
{
	int ret;
	struct kvm_mem_bank *bank;

	list_for_each_entry(bank, &kvm->mem_banks, list) {
		if (type != KVM_MEM_TYPE_ALL && !(bank->type & type))
			continue;

		ret = fun(kvm, bank, data);
		if (ret)
			break;
	}

	return ret;
}

int kvm__recommended_cpus(struct kvm *kvm)
{
	int ret;

	ret = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, KVM_CAP_NR_VCPUS);
	if (ret <= 0)
		/*
		 * api.txt states that if KVM_CAP_NR_VCPUS does not exist,
		 * assume 4.
		 */
		return 4;

	return ret;
}

int kvm__max_cpus(struct kvm *kvm)
{
	int ret;

	ret = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS);
	if (ret <= 0)
		ret = kvm__recommended_cpus(kvm);

	return ret;
}

int kvm__init(struct kvm *kvm)
{
	int ret;

	if (!kvm__arch_cpu_supports_vm()) {
		pr_err("Your CPU does not support hardware virtualization");
		ret = -ENOSYS;
		goto err;
	}

	kvm->sys_fd = open(kvm->cfg.dev, O_RDWR);
	if (kvm->sys_fd < 0) {
		if (errno == ENOENT)
			pr_err("'%s' not found. Please make sure your kernel has CONFIG_KVM "
			       "enabled and that the KVM modules are loaded.", kvm->cfg.dev);
		else if (errno == ENODEV)
			pr_err("'%s' KVM driver not available.\n  # (If the KVM "
			       "module is loaded then 'dmesg' may offer further clues "
			       "about the failure.)", kvm->cfg.dev);
		else
			pr_err("Could not open %s: ", kvm->cfg.dev);

		ret = -errno;
		goto err_free;
	}

	ret = ioctl(kvm->sys_fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION) {
		pr_err("KVM_API_VERSION ioctl");
		ret = -errno;
		goto err_sys_fd;
	}

	kvm->vm_fd = ioctl(kvm->sys_fd, KVM_CREATE_VM, KVM_VM_TYPE);
	if (kvm->vm_fd < 0) {
		pr_err("KVM_CREATE_VM ioctl");
		ret = kvm->vm_fd;
		goto err_sys_fd;
	}

	if (kvm__check_extensions(kvm)) {
		pr_err("A required KVM extension is not supported by OS");
		ret = -ENOSYS;
		goto err_vm_fd;
	}

	kvm__arch_init(kvm, kvm->cfg.hugetlbfs_path, kvm->cfg.ram_size);

	INIT_LIST_HEAD(&kvm->mem_banks);
	kvm__init_ram(kvm);

	if (!kvm->cfg.firmware_filename) {
		if (!kvm__load_kernel(kvm, kvm->cfg.kernel_filename,
				kvm->cfg.initrd_filename, kvm->cfg.real_cmdline))
			die("unable to load kernel %s", kvm->cfg.kernel_filename);
	}

	if (kvm->cfg.firmware_filename) {
		if (!kvm__load_firmware(kvm, kvm->cfg.firmware_filename))
			die("unable to load firmware image %s: %s", kvm->cfg.firmware_filename, strerror(errno));
	} else {
		ret = kvm__arch_setup_firmware(kvm);
		if (ret < 0)
			die("kvm__arch_setup_firmware() failed with error %d\n", ret);
	}

	return 0;

err_vm_fd:
	close(kvm->vm_fd);
err_sys_fd:
	close(kvm->sys_fd);
err_free:
	free(kvm);
err:
	return ret;
}
core_init(kvm__init);

int kvm__pre_copy(struct kvm *kvm, struct pre_copy_context *ctxt)
{
	kvm__fork_trace(ctxt, "core pre-copy: arch state capture start");
	return kvm__arch_pre_copy(kvm, ctxt);
}
core_pre_copy(kvm__pre_copy);

int kvm__post_copy(struct kvm *kvm, struct pre_copy_context *ctxt)
{
	int ret;
	struct kvm_mem_bank *bank, *tmp;

	kvm__fork_trace(ctxt, "child core post-copy: close inherited KVM fds");
	close(kvm->sys_fd);
	close(kvm->vm_fd);

	kvm__fork_trace(ctxt, "child core post-copy: open %s start", kvm->cfg.dev);
	kvm->sys_fd = open(kvm->cfg.dev, O_RDWR);
	if (kvm->sys_fd < 0) {
		if (errno == ENOENT)
			pr_err("'%s' not found. Please make sure your kernel has CONFIG_KVM "
					"enabled and that the KVM modules are loaded.", kvm->cfg.dev);
		else if (errno == ENODEV)
			pr_err("'%s' KVM driver not available.\n  # (If the KVM "
					"module is loaded then 'dmesg' may offer further clues "
					"about the failure.)", kvm->cfg.dev);
		else
			pr_err("Could not open %s: ", kvm->cfg.dev);

		ret = -errno;
		return ret;
	}
	kvm__fork_trace(ctxt, "child core post-copy: open %s complete sys_fd=%d",
		kvm->cfg.dev, kvm->sys_fd);

	kvm__fork_trace(ctxt, "child core post-copy: KVM_CREATE_VM start");
	kvm->vm_fd = ioctl(kvm->sys_fd, KVM_CREATE_VM, KVM_VM_TYPE);
	if (kvm->vm_fd < 0) {
		perror("SADF");
		printf("sys_fd: %d, vm_fd: %d\n", kvm->sys_fd, kvm->vm_fd);
		pr_err("KVM_CREATE_VM ioctl");
		ret = kvm->vm_fd;
		return ret;
	}
	kvm__fork_trace(ctxt, "child core post-copy: KVM_CREATE_VM complete vm_fd=%d",
		kvm->vm_fd);

	kvm__fork_trace(ctxt, "child core post-copy: arch post-copy start");
	kvm__arch_post_copy(kvm, kvm->cfg.hugetlbfs_path, kvm->cfg.ram_size, ctxt);
	kvm__fork_trace(ctxt, "child core post-copy: arch post-copy complete");

	kvm__fork_trace(ctxt, "child core post-copy: clear old mem bank list start");
	list_for_each_entry_safe(bank, tmp, &kvm->mem_banks, list) {
		list_del(&bank->list);
		free(bank);
	}
	kvm__fork_trace(ctxt, "child core post-copy: clear old mem bank list complete");

	kvm__fork_trace(ctxt, "child core post-copy: register RAM slots start");
	kvm__init_ram(kvm);
	kvm__fork_trace(ctxt, "child core post-copy: register RAM slots complete slots=%u",
		kvm->mem_slots);

	return 0;
}
core_post_copy(kvm__post_copy);

int kvm__post_copy_parent(struct kvm *kvm, struct pre_copy_context *ctxt)
{
	struct kvm_mem_bank *bank, *tmp;

	if (!kvm->cfg.cleargmap) {
		kvm__fork_trace(ctxt, "parent post-copy: cleargmap disabled");
		return 0;
	}

	kvm__fork_trace(ctxt, "parent post-copy: rebuild mem bank list start");
	list_for_each_entry_safe(bank, tmp, &kvm->mem_banks, list) {
		list_del(&bank->list);
		free(bank);
	}

	kvm__init_ram(kvm);
	kvm__fork_trace(ctxt, "parent post-copy: rebuild mem bank list complete slots=%u",
		kvm->mem_slots);

	return 0;
}
core_post_copy_parent(kvm__post_copy_parent);

/* RFC 1952 */
#define GZIP_ID1		0x1f
#define GZIP_ID2		0x8b
#define CPIO_MAGIC		"0707"
/* initrd may be gzipped, or a plain cpio */
static bool initrd_check(int fd)
{
	unsigned char id[4];

	if (read_in_full(fd, id, ARRAY_SIZE(id)) < 0)
		return false;

	if (lseek(fd, 0, SEEK_SET) < 0)
		die_perror("lseek");

	return (id[0] == GZIP_ID1 && id[1] == GZIP_ID2) ||
		!memcmp(id, CPIO_MAGIC, 4);
}

bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
		const char *initrd_filename, const char *kernel_cmdline)
{
	bool ret;
	int fd_kernel = -1, fd_initrd = -1;

	fd_kernel = open(kernel_filename, O_RDONLY);
	if (fd_kernel < 0)
		die("Unable to open kernel %s", kernel_filename);

	if (initrd_filename) {
		fd_initrd = open(initrd_filename, O_RDONLY);
		if (fd_initrd < 0)
			die("Unable to open initrd %s", initrd_filename);

		if (!initrd_check(fd_initrd))
			die("%s is not an initrd", initrd_filename);
	}

	ret = kvm__arch_load_kernel_image(kvm, fd_kernel, fd_initrd,
					  kernel_cmdline);

	if (initrd_filename)
		close(fd_initrd);
	close(fd_kernel);

	if (!ret)
		die("%s is not a valid kernel image", kernel_filename);
	return ret;
}

void kvm__dump_mem(struct kvm *kvm, unsigned long addr, unsigned long size, int debug_fd)
{
	unsigned char *p;
	unsigned long n;

	size &= ~7; /* mod 8 */
	if (!size)
		return;

	p = guest_flat_to_host(kvm, addr);

	for (n = 0; n < size; n += 8) {
		if (!host_ptr_in_ram(kvm, p + n)) {
			dprintf(debug_fd, " 0x%08lx: <unknown>\n", addr + n);
			continue;
		}
		dprintf(debug_fd, " 0x%08lx: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			addr + n, p[n + 0], p[n + 1], p[n + 2], p[n + 3],
				  p[n + 4], p[n + 5], p[n + 6], p[n + 7]);
	}
}

void kvm__reboot(struct kvm *kvm)
{
	/* Check if the guest is running */
	if (!kvm->cpus[0] || kvm->cpus[0]->thread == 0)
		return;

	pthread_kill(kvm->cpus[0]->thread, SIGKVMEXIT);
}

void kvm__continue(struct kvm *kvm)
{
	mutex_unlock(&pause_lock);
}

void kvm__pause(struct kvm *kvm)
{
    int i, paused_vcpus = 0;

    mutex_lock(&pause_lock);

    /* Check if the guest is running */
    if (!kvm->cpus || !kvm->cpus[0] || kvm->cpus[0]->thread == 0) {
        return;
    }

    pause_event = eventfd(0, 0);
    if (pause_event < 0)
        die("Failed creating pause notification event");

    for (i = 0; i < kvm->nrcpus; i++) {
        if (kvm->cpus[i]->is_running && kvm->cpus[i]->paused == 0)
            pthread_kill(kvm->cpus[i]->thread, SIGKVMPAUSE);
        else
            paused_vcpus++;
    }

    while (paused_vcpus < kvm->nrcpus) {
        u64 cur_read;

        if (read(pause_event, &cur_read, sizeof(cur_read)) < 0)
            die("Failed reading pause event");

        paused_vcpus += cur_read;
    }

    close(pause_event);
}

void kvm__fork(struct kvm *kvm, bool detach_term, char *new_name)
{
	u64 fork_start_ns = kvm__time_ns();
	static char name[20];
	struct pre_copy_context ctxt;
	int pid;

	ctxt.detach_term = detach_term;
	ctxt.new_name = new_name;
	ctxt.trace_start_ns = fork_start_ns;

	kvm__fork_trace(&ctxt, "start");

	if (init_list__pre_copy(kvm, &ctxt) < 0)
		die ("Pre copy failed");
	kvm__fork_trace(&ctxt, "pre-copy complete");

	fflush(stdout);
	if (kvm->cfg.cleargmap) {
		unsigned freed_slots = kvm->mem_slots;

		kvm__fork_trace(&ctxt, "cleargmap start slots=%u", freed_slots);
		for (unsigned i = 0; i < kvm->mem_slots; i ++) {
			struct kvm_userspace_memory_region mem = (struct kvm_userspace_memory_region) {
				.slot			= i,
				.guest_phys_addr	= 0,
				.memory_size		= 0,
				.userspace_addr		= 0,
			};
			int r = ioctl(kvm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
			if (r < 0)
				die("Failed to free mem");
		}
		kvm->mem_slots = 0;
		kvm__fork_trace(&ctxt, "cleargmap complete slots=%u", freed_slots);
	}
	kvm__fork_trace(&ctxt, "fork syscall start");
	pid = fork();
	if (pid < 0) {
		die("Failed to fork process");
	} else if (pid == 0) {
		// Child
		{
			char logpath[128];
			snprintf(logpath, sizeof(logpath),
				"/tmp/hyperfork_child_%d.log", getpid());
			hyperfork_debug_log = fopen(logpath, "w");
		}
		hyperfork_dbg("===== CHILD FORK START =====");
		hyperfork_dbg("vm_fd=%d sys_fd=%d nrcpus=%d ram_size=%llu",
			kvm->vm_fd, kvm->sys_fd, kvm->nrcpus,
			(unsigned long long)kvm->ram_size);

		kvm__fork_trace(&ctxt, "child fork return");
		sprintf(name, "guest-%u", getpid());
		kvm->cfg.guest_name = name;

		hyperfork_dbg("starting post_copy...");
		if (init_list__post_copy(kvm, &ctxt) < 0)
			die ("Post copy failed");
		kvm__fork_trace(&ctxt, "child post-copy complete");
		hyperfork_dbg("post_copy complete. vm_fd=%d sys_fd=%d",
			kvm->vm_fd, kvm->sys_fd);

		/* Make the PGID unique from the parent such that we can
		 * re-attach the process to a new terminal window. */
		if (setpgid(0,0)) {
			die("Failed to set PGID of child");
		}
		kvm__fork_trace(&ctxt, "child pgid set");

		/* Unpause the child VM */
		hyperfork_dbg("setting vm_state=RUNNING, calling kvm__continue");
		kvm->vm_state = KVM_VMSTATE_RUNNING;
		kvm__continue(kvm);
		kvm__fork_trace(&ctxt, "child vm resumed");

		if (hyperfork_child_octet > 0) {
			char cmd[128];
			snprintf(cmd, sizeof(cmd), "\n/root/vminit.sh %d\n", hyperfork_child_octet);
			hyperfork_dbg("injecting network config: %s", cmd);
			serial8250__inject_string(kvm, cmd);
		}

		kvm__fork_trace_separator(&ctxt, "child fork trace complete");
		hyperfork_dbg("vm resumed, starting VCPU threads via kvm_cmd_run_work");

		/* Start VCPU threads and take over duties of main lkvm run thread.
		 * We have already created another kvm_ipc thread, so here
		 * we just use this one to take over the role of lkvm. Basically
		 * that just means waiting on the VCPU0 thread to exit and then
		 * exiting gracefully. */

		int ret = kvm_cmd_run_work(kvm);
		hyperfork_dbg("kvm_cmd_run_work returned ret=%d", ret);
		kvm_cmd_run_exit(kvm, ret);

		hyperfork_dbg("===== CHILD EXIT =====");
		if (hyperfork_debug_log) fclose(hyperfork_debug_log);
		exit(0);
	} else {
		kvm__fork_trace(&ctxt, "parent fork return child_pid=%d", pid);
		if (init_list__post_copy_parent(kvm, &ctxt) < 0)
			die ("Post copy parent failed");
		kvm__fork_trace(&ctxt, "parent post-copy complete child_pid=%d", pid);
		kvm__fork_trace_separator(&ctxt, "parent fork trace complete");
	}
}

void kvm__notify_paused(void)
{
	u64 p = 1;

	if (write(pause_event, &p, sizeof(p)) < 0)
		die("Failed notifying of paused VCPU.");

	mutex_lock(&pause_lock);
	current_kvm_cpu->paused = 0;
	mutex_unlock(&pause_lock);
}
