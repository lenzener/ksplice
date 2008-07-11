#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/version.h>

#if BITS_PER_LONG == 32
#define ADDR "08lx"
#elif BITS_PER_LONG == 64
#define ADDR "016lx"
#endif
#ifdef KSPLICE_STANDALONE
#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64) &&	\
	LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25) &&		\
	LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
/* Linux 2.6.25 and 2.6.26 apply paravirt replacements to the core
 * kernel but not modules on x86-64.  If we are patching the core
 * kernel, we need to apply the same replacements to our update
 * modules in order for run-pre matching to succeed.
 */
#define KSPLICE_NEED_PARAINSTRUCTIONS 1
#endif
#endif

enum ksplice_state_enum {
	KSPLICE_PREPARING, KSPLICE_APPLIED, KSPLICE_REVERSED
};

struct ksplice_reloc {
	char *sym_name;
	unsigned long blank_addr;
	long blank_offset;
	unsigned long num_sym_addrs;
	unsigned long *sym_addrs;
	int pcrel;
	long addend;
	int size;
	long dst_mask;
	unsigned int rightshift;
};

struct ksplice_size {
	char *name;
	unsigned long size;
	unsigned long thismod_addr;
	unsigned long num_sym_addrs;
	unsigned long *sym_addrs;
};

struct ksplice_patch {
	char *oldstr;
	char *replstr;
	unsigned long oldaddr;
	unsigned long repladdr;
	char *saved;
};

struct module_pack {
	const char *name;
	const char *target;
	unsigned long map_printk;
	struct module *primary;
	enum ksplice_state_enum state;
	const struct ksplice_reloc *primary_relocs, *primary_relocs_end;
	const struct ksplice_size *primary_sizes, *primary_sizes_end;
	const struct ksplice_reloc *helper_relocs, *helper_relocs_end;
	const struct ksplice_size *helper_sizes, *helper_sizes_end;
	struct ksplice_patch *patches, *patches_end;
#ifdef KSPLICE_STANDALONE
#ifdef KSPLICE_NEED_PARAINSTRUCTIONS
	struct paravirt_patch_site
	    *primary_parainstructions, *primary_parainstructions_end,
	    *helper_parainstructions, *helper_parainstructions_end;
#endif
#endif
	struct list_head *reloc_addrmaps;
	struct list_head *reloc_namevals;
	struct list_head *safety_records;
	int debug;
};

struct reloc_nameval {
	struct list_head list;
	char *name;
	unsigned long val;
	enum { NOVAL, TEMP, VAL } status;
};

struct reloc_addrmap {
	struct list_head list;
	unsigned long addr;
	struct reloc_nameval *nameval;
	int pcrel;
	long addend;
	int size;
	long dst_mask;
};

static inline int virtual_address_mapped(unsigned long addr)
{
	pgd_t *pgd;
#ifndef KSPLICE_STANDALONE
	pud_t *pud;
#else /* KSPLICE_STANDALONE */
#ifdef pud_page
	pud_t *pud;
#endif /* pud_page */
#endif /* KSPLICE_STANDALONE */
	pmd_t *pmd;
	pte_t *pte;

	if (addr >= init_mm.start_code && addr < init_mm.end_code)
		return 1;

	pgd = pgd_offset_k(addr);
	if (!pgd_present(*pgd))
		return 0;

#ifndef KSPLICE_STANDALONE
	pud = pud_offset(pgd, addr);
	if (!pud_present(*pud))
		return 0;

	if (pud_large(*pud))
		return 1;

	pmd = pmd_offset(pud, addr);
#else /* KSPLICE_STANDALONE */
#ifdef pud_page
	pud = pud_offset(pgd, addr);
	if (!pud_present(*pud))
		return 0;

#ifdef pud_large
	if (pud_large(*pud))
		return 1;
#endif /* pud_large */

	pmd = pmd_offset(pud, addr);
#else /* pud_page */
	pmd = pmd_offset(pgd, addr);
#endif /* pud_page */
#endif /* KSPLICE_STANDALONE */
	if (!pmd_present(*pmd))
		return 0;

	if (pmd_large(*pmd))
		return 1;

	pte = pte_offset_kernel(pmd, addr);
	if (!pte_present(*pte))
		return 0;

	return 1;
}

struct reloc_nameval *find_nameval(struct module_pack *pack, char *name,
				   int create);
struct reloc_addrmap *find_addrmap(struct module_pack *pack,
				   unsigned long addr);
int handle_myst_reloc(struct module_pack *pack, unsigned long pre_addr,
		      unsigned long run_addr, struct reloc_addrmap *map,
		      int rerun);

struct safety_record {
	struct list_head list;
	unsigned long addr;
	unsigned int size;
	int care;
};

struct candidate_val {
	struct list_head list;
	unsigned long val;
};

#define singular(list) (!list_empty(list) && (list)->next->next == (list))
#define failed_to_find(sym_name) \
	printk(KERN_ERR "ksplice: Failed to find symbol %s at %s:%d\n", \
	       sym_name, __FILE__, __LINE__)

static inline void print_abort(const char *str)
{
	printk(KERN_ERR "ksplice: Aborted. (%s)\n", str);
}

#define ksdebug(pack, level, fmt, ...) \
	do { if ((pack)->debug >= (level)) printk(fmt, ## __VA_ARGS__); } while (0)

int process_ksplice_relocs(struct module_pack *pack,
			   const struct ksplice_reloc *relocs,
			   const struct ksplice_reloc *relocs_end, int pre);
int process_reloc(struct module_pack *pack, const struct ksplice_reloc *r,
		  int pre);
int compute_address(struct module_pack *pack, char *sym_name,
		    struct list_head *vals, int pre);

struct accumulate_struct {
	const char *desired_name;
	struct list_head *vals;
};

#ifdef CONFIG_KALLSYMS
int accumulate_matching_names(void *data, const char *sym_name,
			      unsigned long sym_val);
#ifdef KSPLICE_STANDALONE
int module_on_each_symbol(struct module *mod,
			  int (*fn) (void *, const char *, unsigned long),
			  void *data);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,10)
unsigned long ksplice_kallsyms_expand_symbol(unsigned long off, char *result);
#endif
#endif
int kernel_lookup(const char *name_wlabel, struct list_head *vals);
int other_module_lookup(const char *name_wlabel, struct list_head *vals,
			const char *ksplice_name);
#endif

int add_candidate_val(struct list_head *vals, unsigned long val);
void release_vals(struct list_head *vals);
void set_temp_myst_relocs(struct module_pack *pack, int status_val);
int contains_canary(unsigned long blank_addr, int size, long dst_mask);
int starts_with(const char *str, const char *prefix);
int ends_with(const char *str, const char *suffix);
int label_offset(const char *sym_name);
const char *dup_wolabel(const char *sym_name);

#define clear_list(head, type, member)				\
	do {							\
		struct list_head *_pos, *_n;			\
		list_for_each_safe(_pos, _n, head) {		\
			list_del(_pos);				\
			kfree(list_entry(_pos, type, member));	\
		}						\
	} while (0)

int init_module(void);
void cleanup_module(void);
int init_ksplice_module(struct module_pack *pack);
void cleanup_ksplice_module(struct module_pack *pack);

/* primary */
int activate_primary(struct module_pack *pack);
int resolve_patch_symbols(struct module_pack *pack);
int procfile_read(char *buffer, char **buffer_location, off_t offset,
		  int buffer_length, int *eof, void *data);
int procfile_write(struct file *file, const char *buffer,
		   unsigned long count, void *data);
int __apply_patches(void *packptr);
int __reverse_patches(void *packptr);
int check_each_task(struct module_pack *pack);
int check_task(struct module_pack *pack, struct task_struct *t);
int check_stack(struct module_pack *pack, struct thread_info *tinfo,
		unsigned long *stack);
int check_address_for_conflict(struct module_pack *pack, unsigned long addr);
int valid_stack_ptr(struct thread_info *tinfo, void *p);
int add_dependency_on_address(struct module_pack *pack, unsigned long addr);
int add_patch_dependencies(struct module_pack *pack);
#ifdef KSPLICE_STANDALONE
struct module *module_text_address(unsigned long addr);
int use_module(struct module *a, struct module *b);
#endif

/* helper */
int activate_helper(struct module_pack *pack);
int search_for_match(struct module_pack *pack, const struct ksplice_size *s);
int try_addr(struct module_pack *pack, const struct ksplice_size *s,
	     unsigned long run_addr, unsigned long pre_addr);

#ifdef KSPLICE_STANDALONE
int brute_search_all(struct module_pack *pack, const struct ksplice_size *s);

static inline int brute_search(struct module_pack *pack,
			       const struct ksplice_size *s,
			       void *start, unsigned long len)
{
	unsigned long addr;
	char run, pre;

	for (addr = (unsigned long)start; addr < (unsigned long)start + len;
	     addr++) {
		if (addr % 100000 == 0)
			yield();

		if (!virtual_address_mapped(addr))
			return 1;

		run = *(unsigned char *)(addr);
		pre = *(unsigned char *)(s->thismod_addr);

		if (run != pre)
			continue;

		if (try_addr(pack, s, addr, s->thismod_addr))
			return 0;
	}

	return 1;
}
#endif /* KSPLICE_STANDALONE */
