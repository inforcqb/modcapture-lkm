// SPDX-License-Identifier: GPL-2.0
/*
 * modcapture - dump the image of every kernel module loaded AFTER this one.
 *
 * WHAT IT DOES
 * ------------
 * A kprobe on the kernel's own module-loader entry point
 * (kernel/module.c: load_module()) copies the module image out of kernel
 * memory for every load that reaches it and writes it to
 *
 *      <dump_dir>/<YYYYMMDD>-<HHMMSS>-<uuuuuu>.ko        (default /data/local/tmp)
 *
 * WHY load_module() AND NOT finit_module() / do_init_module()
 * ----------------------------------------------------------
 * Both syscall paths converge on
 *
 *      load_module(struct load_info *info, const char __user *uargs, int flags)
 *
 * with info->hdr already pointing at a verbatim copy of the module FILE and
 * info->len holding its size:
 *
 *   init_module(2)  -> copy_module_from_user()  -> __vmalloc(len) + copy_from_user
 *   finit_module(2) -> kernel_read_file_from_fd(..., READING_MODULE), info.len=size
 *
 * It is the only point in the load path where the original file bytes still
 * exist unmodified.  load_module() itself immediately rewrites section headers
 * in place (rewrite_section_headers()) and frees the copy on the way out
 * (free_copy()), so anything hooked later - do_init_module() included - sees
 * the relocated in-memory image, which is not a loadable .ko any more.
 *
 * Two consequences worth knowing up front:
 *
 *   - a module handed to the kernel through a memfd (the usual Android trick)
 *     is captured exactly as well as one read from a real path, because the
 *     copy into kernel memory has already happened by the time the probe
 *     fires.  A fd-based hook could not do this.
 *   - the capture happens BEFORE validation, so a module that is later
 *     rejected - bad vermagic, unsigned, unknown symbols, wrong KMI - is still
 *     dumped.  That is deliberate: the interesting question is usually "what
 *     did something just try to load", not "what succeeded".
 *
 * WHY THE FILE IS NOT WRITTEN FROM THE PROBE
 * ------------------------------------------
 * A kprobe pre-handler runs with preemption disabled and may not sleep, so it
 * cannot open or write a file.  It memcpy()s the image into a pre-allocated
 * staging slot instead and hands the slot to a workqueue; the file I/O happens
 * there.  Every buffer is allocated in module_init, so the probe performs no
 * allocation of its own.
 *
 * Module loads are serialised by the kernel's module_mutex, but the writes are
 * not, hence more than one slot (see `slots=`).
 *
 * CREDENTIALS
 * -----------
 * The write runs with the credentials of the task that is loading the module
 * (captured in the probe, then applied with override_creds()).  A kworker runs
 * in the kernel SELinux domain, which is not necessarily allowed to create
 * files under /data/local/tmp; the loading task necessarily had access to the
 * .ko and, for a root/su loader, to that directory too.  Mirroring its
 * credentials makes the dump succeed exactly when the loader itself could have
 * written the file, and makes a failure mean what it says.
 *
 * SCOPE / LIMITS
 * --------------
 *   - 5.15 KMI only.  The struct load_info mirror below and the load_module()
 *     prototype are version-checked at compile time.
 *   - Modules loaded before this one are not captured (by definition), and a
 *     module larger than slot_size_mb is counted and skipped rather than
 *     silently truncated.
 *   - Nothing here is hidden: the module shows up in /proc/modules normally.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/elf.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/version.h>

/* The mirror of load_info below, and the load_module() prototype it is read
 * through, are properties of this one kernel version.  Building against
 * anything else would produce a module that silently reads the wrong offsets,
 * so refuse at compile time instead. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0) || \
    LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#error "modcapture targets Linux 5.15 (GKI android13-5.15 / android14-5.15) only"
#endif

#define MCAP_VERSION "1.0.0"

/* ------------------------------------------------------------------ */
/* struct load_info, as kernel/module-internal.h declares it in 5.15   */
/* ------------------------------------------------------------------ */
/*
 * Not includable from a module: module-internal.h is a kernel-private header
 * (it also pulls in asm/module.h and declares mod_verify_sig).  Only the first
 * four members are mirrored, because only hdr/len are needed - and the mirror
 * is asserted to have the right offsets rather than trusted, and cross-checked
 * at runtime by verifying that the pointer really does point at an ELF object
 * (see mcap_looks_like_module()).
 *
 *    struct load_info {
 *            const char *name;            0x00
 *            struct module *mod;          0x08
 *            Elf_Ehdr *hdr;               0x10   <- mirrored
 *            unsigned long len;           0x18   <- mirrored
 *            Elf_Shdr *sechdrs; ...
 *    };
 */
struct mcap_load_info {
	const char *name;
	struct module *mod;
	Elf_Ehdr *hdr;
	unsigned long len;
};

#if defined(CONFIG_64BIT)
BUILD_BUG_ON(offsetof(struct mcap_load_info, hdr) != 0x10);
BUILD_BUG_ON(offsetof(struct mcap_load_info, len) != 0x18);
#endif

/* ------------------------------------------------------------------ */
/* tunables                                                            */
/* ------------------------------------------------------------------ */

static char *dump_dir = "/data/local/tmp";
module_param(dump_dir, charp, 0644);
MODULE_PARM_DESC(dump_dir, "directory the captured .ko files are written to");

static bool capture_enabled = true;
module_param_named(enabled, capture_enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "capture further module loads (default on)");

/* An index line per capture: which file is which module.  The dump name is a
 * timestamp by design, so without this the only way to tell what was captured
 * is to run modinfo on the file afterwards; one line here makes the directory
 * self-describing. */
static bool write_index = true;
module_param_named(index, write_index, bool, 0644);
MODULE_PARM_DESC(index, "append one line per capture to <dump_dir>/modcapture.log");

static unsigned int slot_count = 2;
module_param_named(slots, slot_count, uint, 0444);
MODULE_PARM_DESC(slots, "staging slots (max concurrent dumps in flight), default 2");

static unsigned int slot_size_mb = 16;
module_param_named(slot_size_mb, slot_size_mb, uint, 0444);
MODULE_PARM_DESC(slot_size_mb, "size of one staging slot in MiB, default 16");

#define MCAP_MIN_SLOTS		1
#define MCAP_MAX_SLOTS		16
#define MCAP_MIN_SLOT_MB	1
#define MCAP_MAX_SLOT_MB	256

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

struct mcap_slot {
	struct work_struct work;
	u8 *buf;
	unsigned long cap;
	unsigned long len;
	struct timespec64 ts;
	pid_t pid;
	char comm[TASK_COMM_LEN];
	const struct cred *cred;
	bool busy;		/* owned by a probe or by the work item */
};

static struct mcap_slot *mcap_slots;
static unsigned int mcap_nslots;
static unsigned long mcap_slot_cap;
static DEFINE_SPINLOCK(mcap_lock);

static struct workqueue_struct *mcap_wq;

static atomic64_t mcap_captured = ATOMIC64_INIT(0);
static atomic64_t mcap_dropped = ATOMIC64_INIT(0);
static atomic64_t mcap_oversized = ATOMIC64_INIT(0);
static atomic64_t mcap_write_fail = ATOMIC64_INIT(0);

/* ------------------------------------------------------------------ */
/* validation                                                          */
/* ------------------------------------------------------------------ */

/*
 * Cheap, fault-safe check that `hdr` really is a module image.
 *
 * The read goes through copy_from_kernel_nofault() on purpose: if the struct
 * mirror were ever wrong, hdr would be an arbitrary value, and a plain
 * dereference of it would take the kernel down.  A failed or mismatching read
 * only costs us the capture.
 *
 * ELFCLASS64 + ELFDATA2LSB + ET_REL is what every .ko is; the magic alone
 * would also accept a shared object, and ET_REL is what rules out the
 * do_init_module()-style "already relocated" image.
 */
static bool mcap_looks_like_module(unsigned long hdr, unsigned long len)
{
	Elf64_Ehdr eh;

	if (!hdr || len < sizeof(eh))
		return false;

	if (copy_from_kernel_nofault(&eh, (const void *)hdr, sizeof(eh)))
		return false;

	if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
		return false;
	if (eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_ident[EI_DATA] != ELFDATA2LSB ||
	    eh.e_ident[EI_VERSION] != EV_CURRENT)
		return false;
	if (eh.e_type != ET_REL)
		return false;

	return true;
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */

static void mcap_write_work(struct work_struct *work);

static int mcap_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct mcap_load_info *info;
	struct mcap_slot *slot = NULL;
	unsigned long hdr, len, flags;
	unsigned int i;

	if (unlikely(!capture_enabled))
		return 0;

	/* load_module(struct load_info *info, ...) - x0 is the info pointer. */
	info = (struct mcap_load_info *)regs->regs[0];
	if (unlikely(!info))
		return 0;

	if (copy_from_kernel_nofault(&hdr, &info->hdr, sizeof(hdr)) ||
	    copy_from_kernel_nofault(&len, &info->len, sizeof(len)))
		return 0;

	if (!mcap_looks_like_module(hdr, len))
		return 0;

	if (len > mcap_slot_cap) {
		if (atomic64_inc_return(&mcap_oversized) <= 4)
			pr_warn("module of %lu bytes exceeds slot_size_mb=%uMiB - not captured (raise the parameter)\n",
				len, slot_size_mb);
		return 0;
	}

	spin_lock_irqsave(&mcap_lock, flags);
	for (i = 0; i < mcap_nslots; i++) {
		if (!mcap_slots[i].busy) {
			mcap_slots[i].busy = true;
			slot = &mcap_slots[i];
			break;
		}
	}
	spin_unlock_irqrestore(&mcap_lock, flags);

	if (!slot) {
		atomic64_inc(&mcap_dropped);
		pr_warn_ratelimited("all %u staging slots busy - dropped a capture\n",
				    mcap_nslots);
		return 0;
	}

	/*
	 * The copy has to happen here and cannot be deferred: load_module()
	 * rewrites the section headers in this very buffer a few lines later
	 * and frees it before it returns.  Only the write is deferred.
	 */
	memcpy(slot->buf, (const void *)hdr, len);
	slot->len = len;
	ktime_get_real_ts64(&slot->ts);
	slot->pid = current->pid;
	memcpy(slot->comm, current->comm, TASK_COMM_LEN);
	slot->cred = get_current_cred();

	INIT_WORK(&slot->work, mcap_write_work);
	queue_work_on(WORK_CPU_UNBOUND, mcap_wq, &slot->work);

	atomic64_inc(&mcap_captured);
	return 0;
}

static struct kprobe mcap_kprobe = {
	.symbol_name = "load_module",
	.pre_handler = mcap_pre_handler,
};

/* ------------------------------------------------------------------ */
/* module name, read out of .modinfo in our own private copy           */
/* ------------------------------------------------------------------ */

/*
 * Every bound below is checked because this runs on a buffer that the kernel
 * has NOT validated yet (elf_validity_check() runs after our probe).  A
 * malformed module must produce "no name", never a wild read - the dump of the
 * malformed file is the whole point of capturing it.
 */
static void mcap_modinfo_name(const u8 *buf, unsigned long len,
			      char *out, size_t outsz)
{
	const Elf64_Ehdr *eh;
	const Elf64_Shdr *sh, *strsh, *mi = NULL;
	const char *strtab;
	unsigned long shoff, shnum, shstrndx, i;

	out[0] = '\0';

	if (len < sizeof(*eh))
		return;
	eh = (const Elf64_Ehdr *)buf;

	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		return;
	if (eh->e_shentsize != sizeof(Elf64_Shdr) || !eh->e_shnum)
		return;

	shoff = eh->e_shoff;
	shnum = eh->e_shnum;
	shstrndx = eh->e_shstrndx;

	if (shstrndx >= shnum || shstrndx == SHN_UNDEF)
		return;
	if (shoff > len || shnum > (len - shoff) / sizeof(Elf64_Shdr))
		return;

	sh = (const Elf64_Shdr *)(buf + shoff);
	strsh = &sh[shstrndx];
	if (strsh->sh_offset > len || strsh->sh_size > len - strsh->sh_offset)
		return;
	strtab = (const char *)buf + strsh->sh_offset;

	for (i = 0; i < shnum; i++) {
		const char *nm;

		if (sh[i].sh_name >= strsh->sh_size)
			continue;
		nm = strtab + sh[i].sh_name;
		/* The name has to be NUL-terminated inside the string table. */
		if (!memchr(nm, 0, strsh->sh_size - sh[i].sh_name))
			continue;
		if (!strcmp(nm, ".modinfo")) {
			mi = &sh[i];
			break;
		}
	}

	if (!mi || mi->sh_type == SHT_NOBITS)
		return;
	if (mi->sh_offset > len || mi->sh_size > len - mi->sh_offset)
		return;

	{
		const char *p = (const char *)buf + mi->sh_offset;
		unsigned long left = mi->sh_size;

		while (left) {
			size_t n = strnlen(p, left);

			if (n == left)	/* not NUL-terminated: malformed */
				break;
			if (n > 5 && !memcmp(p, "name=", 5)) {
				size_t c = n - 5;

				if (c >= outsz)
					c = outsz - 1;
				memcpy(out, p + 5, c);
				out[c] = '\0';
				return;
			}
			p += n + 1;
			left -= n + 1;
		}
	}
}

/* ------------------------------------------------------------------ */
/* the deferred write                                                  */
/* ------------------------------------------------------------------ */

static void mcap_index_line(const char *basename, const char *modname,
			    const struct mcap_slot *slot, long written)
{
	struct tm tm;
	char line[320];
	char path[PATH_MAX];
	struct file *f;
	loff_t pos = 0;
	int n;

	time64_to_tm(slot->ts.tv_sec, 0, &tm);
	n = scnprintf(line, sizeof(line),
		      "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ  pid=%d  comm=%s  size=%lu/%lu  name=%s  file=%s\n",
		      (int)tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		      tm.tm_hour, tm.tm_min, tm.tm_sec,
		      (long)(slot->ts.tv_nsec / 1000), slot->pid, slot->comm,
		      slot->len, (unsigned long)written,
		      modname[0] ? modname : "-", basename);

	snprintf(path, sizeof(path), "%s/modcapture.log", dump_dir);
	f = filp_open(path, O_WRONLY | O_CREAT | O_APPEND | O_LARGEFILE, 0644);
	if (IS_ERR(f))
		return;		/* the .ko is what matters; the index is a bonus */
	kernel_write(f, line, n, &pos);
	filp_close(f, NULL);
}

static void mcap_write_work(struct work_struct *work)
{
	struct mcap_slot *slot = container_of(work, struct mcap_slot, work);
	const struct cred *old_cred;
	struct file *f;
	struct tm tm;
	char basename[48];
	char path[PATH_MAX];
	char modname[64];
	loff_t pos = 0;
	ssize_t written;
	int err;

	time64_to_tm(slot->ts.tv_sec, 0, &tm);
	scnprintf(basename, sizeof(basename),
		  "%04d%02d%02d-%02d%02d%02d-%06ld.ko",
		  (int)tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		  tm.tm_hour, tm.tm_min, tm.tm_sec,
		  (long)(slot->ts.tv_nsec / 1000));
	snprintf(path, sizeof(path), "%s/%s", dump_dir, basename);

	mcap_modinfo_name(slot->buf, slot->len, modname, sizeof(modname));

	/* The write is done as whoever caused the load - see the header. */
	old_cred = override_creds(slot->cred);

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
		pr_err("cannot create %s (err %d) - check dump_dir and that the loader may write there\n",
		       path, err);
	} else {
		written = kernel_write(f, slot->buf, slot->len, &pos);
		filp_close(f, NULL);

		if (written == (ssize_t)slot->len) {
			pr_info("captured %ld bytes -> %s (name=%s pid=%d comm=%s)\n",
				(long)slot->len, path,
				modname[0] ? modname : "-", slot->pid, slot->comm);
			if (write_index)
				mcap_index_line(basename, modname, slot, (long)written);
		} else {
			atomic64_inc(&mcap_write_fail);
			pr_err("short write to %s: %ld of %lu bytes\n",
			       path, (long)written, slot->len);
		}
	}

	revert_creds(old_cred);
	put_cred(slot->cred);
	slot->cred = NULL;
	slot->len = 0;

	/* Handed back last, and under the lock, so that a probe taking the slot
	 * next cannot observe the fields above half-written. */
	spin_lock_irq(&mcap_lock);
	slot->busy = false;
	spin_unlock_irq(&mcap_lock);
}

/* ------------------------------------------------------------------ */
/* init / exit                                                         */
/* ------------------------------------------------------------------ */

static int __init mcap_init(void)
{
	unsigned int i;
	int ret;

	if (slot_count < MCAP_MIN_SLOTS || slot_count > MCAP_MAX_SLOTS)
		slot_count = 2;
	if (slot_size_mb < MCAP_MIN_SLOT_MB || slot_size_mb > MCAP_MAX_SLOT_MB)
		slot_size_mb = 16;

	mcap_slot_cap = (unsigned long)slot_size_mb * 1024UL * 1024UL;

	mcap_slots = kcalloc(slot_count, sizeof(*mcap_slots), GFP_KERNEL);
	if (!mcap_slots)
		return -ENOMEM;
	mcap_nslots = slot_count;

	for (i = 0; i < mcap_nslots; i++) {
		mcap_slots[i].buf = vmalloc(mcap_slot_cap);
		if (!mcap_slots[i].buf) {
			ret = -ENOMEM;
			goto err_free;
		}
		mcap_slots[i].cap = mcap_slot_cap;
	}

	mcap_wq = alloc_workqueue("modcapture", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!mcap_wq) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = register_kprobe(&mcap_kprobe);
	if (ret) {
		pr_err("cannot probe load_module (err %d): this kernel has no such symbol, so nothing can be captured\n",
		       ret);
		destroy_workqueue(mcap_wq);
		mcap_wq = NULL;
		goto err_free;
	}

	pr_info("v%s armed on load_module; dumping to %s/%s (%u x %lu MiB staging, %s)\n",
		MCAP_VERSION, dump_dir, "<timestamp>.ko",
		mcap_nslots, mcap_slot_cap >> 20,
		write_index ? "index on" : "index off");
	return 0;

err_free:
	for (i = 0; i < mcap_nslots; i++)
		vfree(mcap_slots[i].buf);
	kfree(mcap_slots);
	mcap_slots = NULL;
	mcap_nslots = 0;
	return ret;
}

static void __exit mcap_exit(void)
{
	unsigned int i;

	unregister_kprobe(&mcap_kprobe);

	/* Anything already queued still has a valid slot, so let it finish
	 * before the buffers go away. */
	if (mcap_wq) {
		flush_workqueue(mcap_wq);
		destroy_workqueue(mcap_wq);
		mcap_wq = NULL;
	}

	for (i = 0; i < mcap_nslots; i++)
		vfree(mcap_slots[i].buf);
	kfree(mcap_slots);
	mcap_slots = NULL;
	mcap_nslots = 0;

	pr_info("unloaded: captured=%lld dropped=%lld oversized=%lld write_fail=%lld\n",
		atomic64_read(&mcap_captured), atomic64_read(&mcap_dropped),
		atomic64_read(&mcap_oversized), atomic64_read(&mcap_write_fail));
}

module_init(mcap_init);
module_exit(mcap_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Capture every kernel module image loaded after this one");
MODULE_AUTHOR("inforcqb");
MODULE_VERSION(MCAP_VERSION);

/*
 * filp_open / kernel_write / override_creds / revert_creds are exported as
 * EXPORT_SYMBOL_NS(sym, ANDROID_GKI_VFS_EXPORT_ONLY), and the Android build
 * rewrites that macro to the long string below before the compiler stringifies
 * it (fs/Makefile and kernel/Makefile: subdir-ccflags-y +=
 * -DANDROID_GKI_VFS_EXPORT_ONLY=VFS_internal_...).  A module that does not
 * import the RESULTING string is refused by the loader with
 *   "module uses symbol (filp_open) from namespace VFS_internal_... but does
 *    not import it"  ->  "Unknown symbol filp_open (err -22)"
 * The long form has to be written out literally: MODULE_IMPORT_NS() stringifies
 * its argument, so passing the macro NAME would import a namespace no kernel
 * creates.
 */
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
