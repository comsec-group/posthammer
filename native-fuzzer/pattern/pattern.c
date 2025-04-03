#include <assert.h>
#include <gsl/gsl_sort.h>
#include <gsl/gsl_statistics.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "../drama/drama.h"

/* #define MULTIBLOCK_ONLY */
/* #define SPLIT_DETECT */
/* #define DEBUG */
/* #define __TARGET_SEED */
/* #define __MICRON_PREMULTI */
/* #define SLEDGEHAMMER */
/* #define RAINBOW2 */

#define KABY_LAKE

#define SPLIT_DETECT_NUM_TRIGGERS_HAMMER 1
/* With scatter for minimal gaps */
#define SPLIT_DETECT_NUM_MISSES_PER_SET_BLK 8
#define SPLIT_DETECT_NUM_MISSES_PER_SET_TRIGGER_MAX 1

#define SPLIT_DETECT_CONST 50
#define SPLIT_DETECT_CONST_ASM ".rept 50\n\t"

#define PATTERN_WARMUP_ROUNDS 16
/* 128 is not enough */
#define PATTERN_NUM_TIMINGS_SINGLE 128
#define PATTERN_NUM_TIMINGS 1024
#define PATTERN_NUM_TRIES 16

#define PATTERN_NUM_TRIES_HAMMER_MIN_POW 1
#define PATTERN_NUM_TRIES_HAMMER_MAX_POW 6

#define PATTERN_BASE_DELAY_TREFI_MIN_POW 0
#define PATTERN_BASE_DELAY_TREFI_MAX_POW 4
#define PATTERN_BASE_DELAY_WINDOW_POW 2

#define PATTERN_CHASE_AMP 128
#define PATTERN_CHASE_AMP_FAST_CHASE 3
#define PATTERN_MAX_WAYNESS 16
#define PATTERN_NUM_CHASES 2

#define PATTERN_HISTORY 32

#define __ALL_RW (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IROTH)

/* Changing this requires changes in main() */
#define PATTERN_NUM_BANKS 1UL
#define PATTERN_NUM_LANES_PER_EVICTION_SET 3

#ifdef SPLIT_DETECT
#define PATTERN_NUM_EVICTION_SETS (PATTERN_NUM_BANKS * 8)
#define PATTERN_RAINBOW_NUM_EVICTION_SETS (PATTERN_NUM_BANKS * 4)
#else
#ifdef DEBUG
#define PATTERN_NUM_EVICTION_SETS (PATTERN_NUM_BANKS * 64)
#define PATTERN_RAINBOW_NUM_EVICTION_SETS (PATTERN_NUM_BANKS * 32)
#else
#define PATTERN_NUM_EVICTION_SETS (PATTERN_NUM_BANKS * 512)
#ifdef KABY_LAKE
#define PATTERN_RAINBOW_NUM_EVICTION_SETS (PATTERN_NUM_BANKS * 96)
#else
#define PATTERN_RAINBOW_NUM_EVICTION_SETS (PATTERN_NUM_BANKS * 160)
#endif
#endif
#endif

#ifdef DEBUG
#define PATTERN_REPRODUCE_TRIES 2
#define PATTERN_SWEEP_TRIES 3
#else
/* New: because we don't care about reproduce so much anymore */
#define PATTERN_REPRODUCE_TRIES 1
#define PATTERN_SWEEP_TRIES 100
#endif

/* Every block is a double chase of two eviction sets */
#define PATTERN_NUM_EVICTION_SETS_PER_BLK 2

/* 8 by design, more doesn't make sense probably */
#define PATTERN_NUM_BLKS_MAX_POW 3
#define PATTERN_NUM_BLKS_MIN 3
#define PATTERN_NUM_BLKS_MAX (1UL << (PATTERN_NUM_BLKS_MAX_POW))
#define PATTERN_NUM_EVICTION_SETS_MIN                                          \
	(PATTERN_NUM_BLKS_MAX * PATTERN_NUM_EVICTION_SETS_PER_BLK)

#define PATTERN_SINGLE_CHASE_MIN_MISSES 10
#define PATTERN_SLICE_INFO_LINE_SIZE 17
#define PATTERN_FAST_CHASE_MISSES 2.0

/* Let's push it and try 12! Reliable up to around 10, found empirically */
#define PATTERN_CHASE_HIGH_DENSITY 12

#define PATTERN_CHASE_MEDIUM_DENSITY 5
#define PATTERN_CHASE_LOW_DENSITY 1

/* How many times 32 in a tREFI for ONE, TWO, or TEN misses_per */
#define PATTERN_SINGLE_LOOP_ONE_MIN 12
#define PATTERN_SINGLE_LOOP_TWO_MIN 10
#define PATTERN_SINGLE_LOOP_TEN_MIN 1
#define PATTERN_SINGLE_LOOP_MAX 20

/*
 * #define BINSEARCH_BALANCE_FAST_SLOW
 * #define BINSEARCH_WIDER_NOP_RANGE
 */

#ifdef BINSEARCH_BALANCE_FAST_SLOW
#define BINSEARCH_WIDER_NOP_RANGE
#endif

/* 128 * 8192, will definitely work, 8 should be safe */
#define PATTERN_NUM_TREFI_HAMMER_FACTOR_MIN_POW 1
#define PATTERN_NUM_TREFI_HAMMER_FACTOR_MAX_POW 8
#define pattern_num_trefi_hammer(pow) ((1UL << (pow)) * 8192)
#define PATTERN_NUM_COUNTS_SYNC 10

#define PATTERN_SYNC_HALVES 10
#define PATTERN_SYNC_MAX_NOPS 1000000
#define PATTERN_SYNC_ACCURACY 0.05

#define PATTERN_SYNC_MAX_REFS 1
#define PATTERN_SYNC_MAX_REFS_MULTIBLOCK 14
#define sync_max_refs(refs) (refs * (1 + PATTERN_SYNC_ACCURACY))

#define PATTERN_SYNC_SUCCESSES 32

#define PATTERN_NUM_LOOPS_DETERMINE_REFS 64

/* Less won't work for repr. */
#define PATTERN_NUM_LOOPS_SYNC 1024
#define PATTERN_NUM_TRIES_SYNC 16384
#define PATTERN_NUM_TRIES_REINSTANTIATE 16

#define MISSES_PER_SET_MIN 1UL
#define MISSES_PER_SET_MAX 8UL
#define __MISSES_PER_SET_MAX_RAINBOW 12UL
#define MISSES_PER_SET_MAX_BITS 4
#define MISSES_PER_SET_MAX_HEX 0xfUL

#define REPS_OF_BLK_MAX 32UL
#define REPS_OF_BLK_MIN 1UL

#define REPS_PER_REF_MIN 6UL
#define REPS_PER_REF_MAX 160UL
#define __REPS_PER_REF_LOW_MAX 16UL
/* #define reps_per_ref_min(nu) (nu + 1) */

#define PATTERN_NOP_RANGE 3

#define PATTERN_SUBITER_MAX (MISSES_PER_SET_MAX * PATTERN_NOP_RANGE)
#define PATTERN_SUBITER_MAX_BITS 6
#define PATTERN_SUBITER_MAX_HEX 0x3fUL

#define printp(...)                                                            \
	do {                                                                   \
		fprintf(stdout, "%s: ", __func__);                             \
		fprintf(stdout, __VA_ARGS__);                                  \
	} while (0)

struct args {
	char *bank_stats_file;
	char *slice_info_file;
	char *pat_info_file;

	char *snapshot;
};

/* Just run cpuid and get it */
struct llc_info {
	/* All sizes in bytes */
	size_t wayness;
	size_t total_num_sets;
	size_t num_set_bits; /* Computed manually: log2(total_sets / cores) */

	size_t total_cache_size;
	size_t line_size;
	size_t num_line_bits;

	size_t num_cpu_cores; /* AKA: slices */
};

static size_t cpu_nops_per_ref;

static struct {
	char *snapshot;
	size_t pre_seed;
	unsigned long pfn;
} replay_info;

struct slice_info {
	unsigned long *masks;
	size_t size;
};

struct eviction_set {
	unsigned long *addrs;
	size_t size;
	ssize_t bank;

	struct eviction_set *next;
};

struct chase {
	size_t size;

	size_t *flips_around_index;
	/*
	 * At which indices are misses? Makes it easier to permute the
	 * pattern
	 */
	bool *miss_at_index;

	size_t misses_per_set;
	size_t hit_reps; /* Typically one */
	size_t non_uniformity; /* Copied from the block */

	/* How the misses/hits are ordered, copied from pattern */
	bool gather;
	bool interleave;
	bool rainbow;

	struct {
		unsigned long *addrs;
		ssize_t bank;
	} instance;

	struct {
		size_t r;
	} rotation;
};

struct data_pattern {
	size_t index;

	unsigned char a;
	unsigned char v;
};

/* Bank/row */
struct br {
	size_t b;
	size_t r;
};

enum chase_types { EQUAL, MIXED_NONZERO, MIXED_ZERO, NUM_CHASE_TYPES };

struct refblock {
	size_t num_flips;

	enum chase_types ct;
	struct chase cs[PATTERN_NUM_CHASES];

	struct data_pattern dp;

	/* nops_per... e.g. 50, per_ref */
	ssize_t nops_per_per_ref;
	size_t __press_nops_per;

	/* How many times go over each chase per */
	size_t reps_per_ref;

	size_t __non_uniformity;

	/* How many times to repeat the whole block? */
	size_t reps_of_blk;

	/* Might be pointing elsewhere! */
	size_t id;
	struct refblock *self;
};

/*
 * Create a parameter set that works. Mutate from there. We distinguish between
 * three classes of parameters:
 *
 * - Searching parameters. These should change and then stop changing upon a
 *   bit flip
 * - Pattern parameters (incl. data pattern). These should be fixed from the
 *   beginning until a bit flip is found and then start changing (of course
 *   only if you know that they work, otherwise these should also change)
 *
 * If num_loops == num_fast_loops, then slow == NULL because this is a single
 * pattern. Otherwise, it's a double one, and one loop (num_loops) =
 * num_fast_loops + num_slow_loops
 */

enum set_ids { F, S, NUM_SET_IDS };

struct pattern {
	size_t seed;

	size_t num_blks;

	size_t *sets;
	size_t num_sets;

#ifdef SLEDGEHAMMER
	struct {
		bool enabled;
		size_t sides;
		size_t banks;
		size_t set;
	} sledgehammer;
#endif

	struct {
		bool enabled;
		size_t nops_per;
		double num_refs;
		double extra;
	} multiblock;

	struct {
		bool enabled;
		struct refblock blk;
	} trigger;

	bool gather;
	bool unique;
	bool interleave;
	bool rainbow;

	size_t press_nops_per;

	size_t num_tries_hammer;
	size_t num_trefi_factor;
	size_t base_delay_trefi;
	size_t hc_index;

	struct refblock blks[PATTERN_NUM_BLKS_MAX];

	struct pattern *next;
};

enum fuzzer_modes { FUZZ, REPRODUCE, SWEEP };

struct fuzzer {
	struct {
		enum fuzzer_modes mode;

		size_t reproduce_index;

		size_t flips_total;
		size_t rounds_one_flip;
		size_t victim_rows;
		size_t tries; /* Number of hammer attempts (rounds) */
	} state;

	int fd; /* for exporting bit flip info */

	struct pattern *ps;

	size_t *seed_history;
	ssize_t seed_history_head;
};

#define __WAYNESS 16

static size_t __KABY_SLICE_MASKS[] = { 0x1b5f575440UL, 0x2eb5faa880UL,
				       0x3cccc93100UL };
#define __KABY_SLICE_MASKS_SIZE (sizeof(__KABY_SLICE_MASKS) / sizeof(size_t))

struct llc_info cn104 = { .wayness = __WAYNESS,
			  .total_num_sets = 12288,
			  .num_set_bits = 10,
			  .total_cache_size = 12582912,
			  .line_size = 64,
			  .num_line_bits = 6,
			  .num_cpu_cores = 12 };

#define single_chase_into_t(first, t, c, n)                                    \
	asm volatile("mfence\n\t" /* globally visible loads and stores */      \
		     "lfence\n\t" /* mfence and anything else done */          \
		     "mov $0x0, %%rcx\n\t"                                     \
		     "rdpmc\n\t"                                               \
		     "mov %%rax, %%r9\n\t"                                     \
		     "rdtsc\n\t"                                               \
		     "mov %%rax, %%r8\n\t"                                     \
		     "lfence\n\t" /* rdtsc/rdpmc done */                       \
                                                                               \
		     "mov %3, %%r10\n\t" /* Copy loop iter. */                 \
                                                                               \
		     "mov (%2), %2\n\t"                                        \
		     "dec %%r10\n\t"                                           \
		     "cmp $0x0, %%r10\n\t"                                     \
		     "jne .-10\n\t"                                            \
                                                                               \
		     "mfence\n\t" /* globally visible loads and stores */      \
		     "lfence\n\t" /* local instructions done */                \
		     "rdtsc\n\t"                                               \
		     "sub %%r8, %%rax\n\t"                                     \
		     "mov %%rax, %%rbx\n\t"                                    \
		     "mov $0x0, %%rcx\n\t"                                     \
		     "rdpmc\n\t"                                               \
		     "sub %%r9, %%rax\n\t"                                     \
		     : "+b"(t), "+a"(c), "+r"(first)                           \
		     : "r"(n)                                                  \
		     : "rcx", "rdx", "r8", "r9", "r10", "memory");

static inline void asm_hammer_evict_loop_single_block_remaining_rainbow2(
	unsigned long hxa, unsigned long hxb, unsigned long hya,
	unsigned long hyb, size_t amp)
{
	asm volatile("mov %[amp], %%rax\n\t"
		     "1:\n\t"

		     ".rept 2\n\t"
		     ".rept 8\n\t"
		     "mov (%[hxa]), %[hxa]\n\t"
		     "mov (%[hya]), %[hya]\n\t"

		     "mov (%[hxb]), %[hxb]\n\t"
		     "mov (%[hyb]), %[hyb]\n\t"
		     ".endr\n\t"

		     "xor %[hya], %[hxa]\n\t"
		     "xor %[hxa], %[hya]\n\t"
		     "xor %[hya], %[hxa]\n\t"

		     "xor %[hxb], %[hya]\n\t"
		     "xor %[hya], %[hxb]\n\t"
		     "xor %[hxb], %[hya]\n\t"

		     "xor %[hyb], %[hxb]\n\t"
		     "xor %[hxb], %[hyb]\n\t"
		     "xor %[hyb], %[hxb]\n\t"

		     "xor %[hxa], %[hyb]\n\t"
		     "xor %[hyb], %[hxa]\n\t"
		     "xor %[hxa], %[hyb]\n\t"
		     ".endr\n\t"

		     "dec %%rax\n\t"
		     "jnz 1b\n\t"
		     :
		     : [amp] "r"(amp), [hxa] "r"(hxa), [hya] "r"(hya),
		       [hxb] "r"(hxb), [hyb] "r"(hyb)
		     : "rax", "rbx", "rcx");
}

static inline void asm_hammer_evict_loop_single_block_remaining_rainbow(
	unsigned long hxa, unsigned long hxb, unsigned long hxc,
	unsigned long hxd, unsigned long hya, unsigned long hyb,
	unsigned long hyc, unsigned long hyd, size_t amp)
{
	asm volatile("mov %[amp], %%rax\n\t"
		     "1:\n\t"

		     ".rept 2\n\t"
		     ".rept 4\n\t"
		     "mov (%[hxa]), %[hxa]\n\t"
		     "mov (%[hya]), %[hya]\n\t"

		     "mov (%[hxb]), %[hxb]\n\t"
		     "mov (%[hyb]), %[hyb]\n\t"

		     "mov (%[hxc]), %[hxc]\n\t"
		     "mov (%[hyc]), %[hyc]\n\t"

		     "mov (%[hxd]), %[hxd]\n\t"
		     "mov (%[hyd]), %[hyd]\n\t"
		     ".endr\n\t"

		     "xor %[hya], %[hxa]\n\t"
		     "xor %[hxa], %[hya]\n\t"
		     "xor %[hya], %[hxa]\n\t"

		     "xor %[hxb], %[hya]\n\t"
		     "xor %[hya], %[hxb]\n\t"
		     "xor %[hxb], %[hya]\n\t"

		     "xor %[hyb], %[hxb]\n\t"
		     "xor %[hxb], %[hyb]\n\t"
		     "xor %[hyb], %[hxb]\n\t"

		     "xor %[hxc], %[hyb]\n\t"
		     "xor %[hyb], %[hxc]\n\t"
		     "xor %[hxc], %[hyb]\n\t"

		     "xor %[hyc], %[hxc]\n\t"
		     "xor %[hxc], %[hyc]\n\t"
		     "xor %[hyc], %[hxc]\n\t"

		     "xor %[hxd], %[hyc]\n\t"
		     "xor %[hyc], %[hxd]\n\t"
		     "xor %[hxd], %[hyc]\n\t"

		     "xor %[hyd], %[hxd]\n\t"
		     "xor %[hxd], %[hyd]\n\t"
		     "xor %[hyd], %[hxd]\n\t"

		     "xor %[hxa], %[hyd]\n\t"
		     "xor %[hyd], %[hxa]\n\t"
		     "xor %[hxa], %[hyd]\n\t"
		     ".endr\n\t"

		     "dec %%rax\n\t"
		     "jnz 1b\n\t"
		     :
		     : [amp] "r"(amp), [hxa] "r"(hxa), [hya] "r"(hya),
		       [hxb] "r"(hxb), [hyb] "r"(hyb), [hxc] "r"(hxc),
		       [hyc] "r"(hyc), [hxd] "r"(hxd), [hyd] "r"(hyd)
		     : "rax");
}

static inline void
asm_hammer_evict_loop_single_block_remaining(unsigned long hx, unsigned long hy,
					     size_t amp)
{
	asm volatile("mov %[amp], %%rax\n\t"
		     "0:\n\t"
		     ".rept 32\n\t"
		     "mov (%[hx]), %[hx]\n\t"
		     "mov (%[hy]), %[hy]\n\t"
		     ".endr\n\t"
		     "dec %%rax\n\t"
		     "jnz 0b\n\t"
		     :
		     : [amp] "r"(amp), [hx] "r"(hx), [hy] "r"(hy)
		     : "rax");
}

static inline void asm_hammer_evict_loop_single_block_rainbow2(
	unsigned long hxa, unsigned long hxb, unsigned long hya,
	unsigned long hyb, size_t amp, size_t nops_per)
{
	asm volatile("mov %[nops_per], %%rax\n\t"
		     "0:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
		     ".endr\n\t"
		     "dec %%rax\n\t"
		     "jnz 0b\n\t"

		     "mov %[amp], %%rax\n\t"
		     "1:\n\t"

		     ".rept 2\n\t"
		     ".rept 8\n\t"
		     "mov (%[hxa]), %[hxa]\n\t"
		     "mov (%[hya]), %[hya]\n\t"

		     "mov (%[hxb]), %[hxb]\n\t"
		     "mov (%[hyb]), %[hyb]\n\t"
		     ".endr\n\t"

		     "xor %[hya], %[hxa]\n\t"
		     "xor %[hxa], %[hya]\n\t"
		     "xor %[hya], %[hxa]\n\t"

		     "xor %[hxb], %[hya]\n\t"
		     "xor %[hya], %[hxb]\n\t"
		     "xor %[hxb], %[hya]\n\t"

		     "xor %[hyb], %[hxb]\n\t"
		     "xor %[hxb], %[hyb]\n\t"
		     "xor %[hyb], %[hxb]\n\t"

		     "xor %[hxa], %[hyb]\n\t"
		     "xor %[hyb], %[hxa]\n\t"
		     "xor %[hxa], %[hyb]\n\t"
		     ".endr\n\t"

		     "dec %%rax\n\t"
		     "jnz 1b\n\t"
		     :
		     : [amp] "r"(amp), [nops_per] "r"(nops_per), [hxa] "r"(hxa),
		       [hya] "r"(hya), [hxb] "r"(hxb), [hyb] "r"(hyb)
		     : "rax", "rbx", "rcx");
}

static inline void asm_hammer_evict_loop_single_block_rainbow(
	unsigned long hxa, unsigned long hxb, unsigned long hxc,
	unsigned long hxd, unsigned long hya, unsigned long hyb,
	unsigned long hyc, unsigned long hyd, size_t amp, size_t nops_per)
{
	asm volatile(
		"mov %[nops_per], %%rax\n\t"
		"0:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
		".endr\n\t"
		"dec %%rax\n\t"
		"jnz 0b\n\t"

		"mov %[amp], %%rax\n\t"
		"1:\n\t"

		".rept 2\n\t"
		".rept 4\n\t"
		"mov (%[hxa]), %[hxa]\n\t"
		"mov (%[hya]), %[hya]\n\t"

		"mov (%[hxb]), %[hxb]\n\t"
		"mov (%[hyb]), %[hyb]\n\t"

		"mov (%[hxc]), %[hxc]\n\t"
		"mov (%[hyc]), %[hyc]\n\t"

		"mov (%[hxd]), %[hxd]\n\t"
		"mov (%[hyd]), %[hyd]\n\t"
		".endr\n\t"

		"xor %[hya], %[hxa]\n\t"
		"xor %[hxa], %[hya]\n\t"
		"xor %[hya], %[hxa]\n\t"

		"xor %[hxb], %[hya]\n\t"
		"xor %[hya], %[hxb]\n\t"
		"xor %[hxb], %[hya]\n\t"

		"xor %[hyb], %[hxb]\n\t"
		"xor %[hxb], %[hyb]\n\t"
		"xor %[hyb], %[hxb]\n\t"

		"xor %[hxc], %[hyb]\n\t"
		"xor %[hyb], %[hxc]\n\t"
		"xor %[hxc], %[hyb]\n\t"

		"xor %[hyc], %[hxc]\n\t"
		"xor %[hxc], %[hyc]\n\t"
		"xor %[hyc], %[hxc]\n\t"

		"xor %[hxd], %[hyc]\n\t"
		"xor %[hyc], %[hxd]\n\t"
		"xor %[hxd], %[hyc]\n\t"

		"xor %[hyd], %[hxd]\n\t"
		"xor %[hxd], %[hyd]\n\t"
		"xor %[hyd], %[hxd]\n\t"

		"xor %[hxa], %[hyd]\n\t"
		"xor %[hyd], %[hxa]\n\t"
		"xor %[hxa], %[hyd]\n\t"
		".endr\n\t"

		"dec %%rax\n\t"
		"jnz 1b\n\t"
		:
		: [amp] "r"(amp), [nops_per] "r"(nops_per), [hxa] "r"(hxa),
		  [hya] "r"(hya), [hxb] "r"(hxb), [hyb] "r"(hyb),
		  [hxc] "r"(hxc), [hyc] "r"(hyc), [hxd] "r"(hxd), [hyd] "r"(hyd)
		: "rax");
}

/* Normal hammer, no trigger */
static inline void
asm_hammer_evict_loop_single_block(unsigned long hx, unsigned long hy,
				   size_t amp, size_t nops_per,
				   size_t press_nops_per, bool interleave)
{
	if (press_nops_per) {
		asm volatile("mov %[nops_per], %%rax\n\t"
			     "0:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%rax\n\t"
			     "jnz 0b\n\t"

			     "mov %[amp], %%rax\n\t"
			     "1:\n\t"

			     ".rept 32\n\t"
			     "mov (%[hx]), %[hx]\n\t"
			     "mov %[press_nops_per], %%rbx\n\t"
			     "2:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%rbx\n\t"
			     "jnz 2b\n\t"
			     ".endr\n\t"

			     ".rept 32\n\t"
			     "mov (%[hy]), %[hy]\n\t"
			     "mov %[press_nops_per], %%rbx\n\t"
			     "3:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%rbx\n\t"
			     "jnz 3b\n\t"
			     ".endr\n\t"

			     "dec %%rax\n\t"
			     "jnz 1b\n\t"
			     :
			     : [amp] "r"(amp), [nops_per] "r"(nops_per),
			       [press_nops_per] "r"(press_nops_per),
			       [hx] "r"(hx), [hy] "r"(hy)
			     : "rax", "rbx");
	} else if (interleave) {
		asm volatile("mov %[nops_per], %%rax\n\t"
			     "0:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%rax\n\t"
			     "jnz 0b\n\t"

			     "mov %[amp], %%rax\n\t"
			     "1:\n\t"
			     ".rept 32\n\t"
			     "mov (%[hx]), %[hx]\n\t"
			     "mov (%[hy]), %[hy]\n\t"
			     ".endr\n\t"
			     "dec %%rax\n\t"
			     "jnz 1b\n\t"
			     :
			     : [amp] "r"(amp), [nops_per] "r"(nops_per),
			       [hx] "r"(hx), [hy] "r"(hy)
			     : "rax");
	} else {
		asm volatile("mov %[nops_per], %%rax\n\t"
			     "0:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%rax\n\t"
			     "jnz 0b\n\t"

			     "mov %[amp], %%rax\n\t"
			     "1:\n\t"

			     ".rept 32\n\t"
			     "mov (%[hx]), %[hx]\n\t"
			     ".endr\n\t"

			     ".rept 32\n\t"
			     "mov (%[hy]), %[hy]\n\t"
			     ".endr\n\t"

			     "dec %%rax\n\t"
			     "jnz 1b\n\t"
			     :
			     : [amp] "r"(amp), [nops_per] "r"(nops_per),
			       [hx] "r"(hx), [hy] "r"(hy)
			     : "rax");
	}
}

/* nx, ny should be an 8-misses per set set, so we get one miss and one hit
 * each time */
static inline void asm_hammer_evict_loop_single_block_trigger(
	unsigned long hx, unsigned long hy, unsigned long tx, unsigned long ty,
	size_t amp, size_t num_triggers, size_t nops_per)
{
	asm volatile("mov %[num_triggers], %%rax\n\t"
		     "0:\n\t"

		     ".rept 32\n\t"
		     "mov (%[tx]), %[tx]\n\t"
		     "mov (%[ty]), %[ty]\n\t"
		     ".endr\n\t"

		     "mov %[nops_per], %%rbx\n\t"
		     "1:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
		     ".endr\n\t"
		     "dec %%rbx\n\t"
		     "jnz 1b\n\t"

		     "dec %%rax\n\t"
		     "jnz 0b\n\t"

		     "mov %[amp], %%rax\n\t"
		     "2:\n\t"
		     ".rept 32\n\t"
		     "mov (%[hx]), %[hx]\n\t"
		     "mov (%[hy]), %[hy]\n\t"
		     ".endr\n\t"
		     "dec %%rax\n\t"
		     "jnz 2b\n\t"
		     :
		     : [amp] "r"(amp), [num_triggers] "r"(num_triggers),
		       [nops_per] "r"(nops_per), [hx] "r"(hx), [hy] "r"(hy),
		       [tx] "r"(tx), [ty] "r"(ty)
		     : "rax", "rbx");
}

static struct pattern *pattern_pop(struct pattern **ps)
{
	struct pattern *head = *ps;

	if (!head) return NULL;

	*ps = head->next;
	head->next = NULL;

	return head;
}

static struct pattern *pattern_new(struct pattern **ps)
{
	struct pattern *head = *ps;
	struct pattern *p = calloc(1, sizeof(struct pattern));

	if (!head) {
		*ps = p;
	} else {
		while (head->next) {
			head = head->next;
		}

		head->next = p;
	}

	return p;
}

/*
 * If you think of those A, B and A, B, A, C patterns, then reps_per_ref will
 * say how often at least A is repeated. In dependent of nu!
 */
static size_t reps_per_ref_min(size_t nu)
{
	size_t l = 1;

	while (l * (nu + 1) < REPS_PER_REF_MIN) {
		l++;
	}

	assert(l * (nu + 1) >= REPS_PER_REF_MIN);

	return l * (nu + 1);
}

static double gsl_stats_median(double *data, const size_t stride,
			       const size_t n)
{
	gsl_sort(data, stride, n);
	return gsl_stats_median_from_sorted_data(data, stride, n);
}

static size_t reps_per_ref_reduce(size_t reps_per_ref, size_t nu)
{
	assert(reps_per_ref);
	assert(reps_per_ref % (nu + 1) == 0);
	return reps_per_ref - (nu + 1);
}

enum pattern_part { DENSE, SPARSE, BOTH };

static void asm_measure_single_block_nops_with_trigger(
	unsigned long hx, unsigned long hy, unsigned long tx, unsigned long ty,
	size_t amp, size_t num_triggers, size_t nops_per, unsigned long *misses,
	unsigned long *cycles, enum pattern_part pp)
{
	unsigned long __misses = 0;
	unsigned long __cycles = 0;

	assert(num_triggers);

	if (pp == DENSE) {
		asm volatile("mov %[num_triggers], %%r10\n\t"
			     "1:\n\t"

			     ".rept 32\n\t"
			     "mov (%[tx]), %[tx]\n\t"
			     "mov (%[ty]), %[ty]\n\t"
			     ".endr\n\t"

			     "mov %[nops_per], %%r11\n\t"
			     "2:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%r11\n\t"
			     "jnz 2b\n\t"

			     "dec %%r10\n\t"
			     "jnz 1b\n\t"

			     "mfence\n\t"
			     "lfence\n\t"
			     "mov $0x0, %%rcx\n\t"
			     "rdpmc\n\t"
			     "mov %%rax, %%r9\n\t"
			     "rdtsc\n\t"
			     "mov %%rax, %%r8\n\t"

			     "mov %[amp], %%rax\n\t"
			     "3:\n\t"
			     ".rept 32\n\t"
			     "mov (%[hx]), %[hx]\n\t"
			     "mov (%[hy]), %[hy]\n\t"
			     ".endr\n\t"
			     "dec %%rax\n\t"
			     "jnz 3b\n\t"

			     "lfence\n\t"
			     "rdtsc\n\t"
			     "sub %%r8, %%rax\n\t"
			     "mov %%rax, %%rbx\n\t"
			     "mov $0x0, %%rcx\n\t"
			     "rdpmc\n\t"
			     "sub %%r9, %%rax\n\t"
			     : "+b"(__cycles),
			       "+a"(__misses), [tx] "+r"(tx), [ty] "+r"(ty)
			     : [amp] "r"(amp), [nops_per] "r"(nops_per),
			       [num_triggers] "r"(num_triggers), [hx] "r"(hx),
			       [hy] "r"(hy)
			     : "rcx", "rdx", "r8", "r9", "r10", "r11");
	} else if (pp == SPARSE) {
		asm volatile("mfence\n\t"
			     "lfence\n\t"
			     "mov $0x0, %%rcx\n\t"
			     "rdpmc\n\t"
			     "mov %%rax, %%r9\n\t"
			     "rdtsc\n\t"
			     "mov %%rax, %%r8\n\t"

			     "mov %[num_triggers], %%r10\n\t"
			     "1:\n\t"

			     ".rept 32\n\t"
			     "mov (%[tx]), %[tx]\n\t"
			     "mov (%[ty]), %[ty]\n\t"
			     ".endr\n\t"

			     "mov %[nops_per], %%r11\n\t"
			     "2:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%r11\n\t"
			     "jnz 2b\n\t"

			     "dec %%r10\n\t"
			     "jnz 1b\n\t"

			     "lfence\n\t"
			     "rdtsc\n\t"
			     "sub %%r8, %%rax\n\t"
			     "mov %%rax, %%rbx\n\t"
			     "mov $0x0, %%rcx\n\t"
			     "rdpmc\n\t"
			     "sub %%r9, %%rax\n\t"

			     "mov %[amp], %%r9\n\t"
			     "3:\n\t"
			     ".rept 32\n\t"
			     "mov (%[hx]), %[hx]\n\t"
			     "mov (%[hy]), %[hy]\n\t"
			     ".endr\n\t"
			     "dec %%r9\n\t"
			     "jnz 3b\n\t"
			     : "+b"(__cycles),
			       "+a"(__misses), [tx] "+r"(tx), [ty] "+r"(ty)
			     : [amp] "r"(amp), [nops_per] "r"(nops_per),
			       [num_triggers] "r"(num_triggers), [hx] "r"(hx),
			       [hy] "r"(hy)
			     : "rcx", "rdx", "r8", "r9", "r10", "r11");
	} else if (pp == BOTH) {
		asm volatile("mfence\n\t"
			     "lfence\n\t"
			     "mov $0x0, %%rcx\n\t"
			     "rdpmc\n\t"
			     "mov %%rax, %%r9\n\t"
			     "rdtsc\n\t"
			     "mov %%rax, %%r8\n\t"

			     "mov %[num_triggers], %%r10\n\t"
			     "1:\n\t"

			     ".rept 32\n\t"
			     "mov (%[tx]), %[tx]\n\t"
			     "mov (%[ty]), %[ty]\n\t"
			     ".endr\n\t"

			     "mov %[nops_per], %%r11\n\t"
			     "2:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
			     ".endr\n\t"
			     "dec %%r11\n\t"
			     "jnz 2b\n\t"

			     "dec %%r10\n\t"
			     "jnz 1b\n\t"

			     "mov %[amp], %%rax\n\t"
			     "3:\n\t"
			     ".rept 32\n\t"
			     "mov (%[hx]), %[hx]\n\t"
			     "mov (%[hy]), %[hy]\n\t"
			     ".endr\n\t"
			     "dec %%rax\n\t"
			     "jnz 3b\n\t"

			     "lfence\n\t"
			     "rdtsc\n\t"
			     "sub %%r8, %%rax\n\t"
			     "mov %%rax, %%rbx\n\t"
			     "mov $0x0, %%rcx\n\t"
			     "rdpmc\n\t"
			     "sub %%r9, %%rax\n\t"
			     : "+b"(__cycles),
			       "+a"(__misses), [tx] "+r"(tx), [ty] "+r"(ty)
			     : [amp] "r"(amp), [nops_per] "r"(nops_per),
			       [num_triggers] "r"(num_triggers), [hx] "r"(hx),
			       [hy] "r"(hy)
			     : "rcx", "rdx", "r8", "r9", "r10", "r11");
	}

	*misses = __misses;
	*cycles = __cycles;
}

/* Turns out to be quite reliable! */
static unsigned long __get_misses(void)
{
	unsigned long misses = 0;

	asm volatile("mfence\n\t"
		     "lfence\n\t"
		     "mov $0x0, %%rcx\n\t"
		     "rdpmc\n\t"
		     : "+a"(misses)::"rcx");

	return misses;
}

static size_t __refblock_exp_misses(struct refblock *blk)
{
	size_t y = 0;

	/* The 2x comes from the fact that we do 32 accesses per chase, not 16 */
	for (size_t i = 0; i < PATTERN_NUM_CHASES; i++) {
		y += blk->reps_per_ref * blk->reps_of_blk *
		     (2 * blk->cs[i].misses_per_set);
	}

	return y;
}

static size_t pattern_exp_misses(struct pattern *p)
{
	size_t exp = 0;

	for (size_t i = 0; i < p->num_blks; i++) {
		struct refblock *blk = p->blks[i].self;
		exp += __refblock_exp_misses(blk);
	}

	if (p->trigger.enabled) {
		for (size_t i = 0; i < PATTERN_NUM_CHASES; i++) {
			exp += 2 * p->trigger.blk.cs[i].misses_per_set;
		}
	}

	return exp;
}

static void asm_measure_single_block_no_nops(unsigned long hx, unsigned long hy,
					     size_t amp, size_t nops_per,
					     unsigned long *misses,
					     unsigned long *cycles)
{
	unsigned long __misses = 0;
	unsigned long __cycles = 0;

	assert(nops_per);

	asm volatile("mov %[nops_per], %%rax\n\t"
		     "0:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
		     ".endr\n\t"
		     "dec %%rax\n\t"
		     "jnz 0b\n\t"

		     "mfence\n\t"
		     "lfence\n\t"
		     "mov $0x0, %%rcx\n\t"
		     "rdpmc\n\t"
		     "mov %%rax, %%r9\n\t"
		     "rdtsc\n\t"
		     "mov %%rax, %%r8\n\t"

		     "mov %[amp], %%rax\n\t"
		     "1:\n\t"
		     ".rept 32\n\t"
		     "mov (%[hx]), %[hx]\n\t"
		     "mov (%[hy]), %[hy]\n\t"
		     ".endr\n\t"
		     "dec %%rax\n\t"
		     "jnz 1b\n\t"

		     "lfence\n\t" /* local instructions done */
		     "rdtsc\n\t"
		     "sub %%r8, %%rax\n\t"
		     "mov %%rax, %%rbx\n\t"
		     "mov $0x0, %%rcx\n\t"
		     "rdpmc\n\t"
		     "sub %%r9, %%rax\n\t"
		     : "+b"(__cycles), "+a"(__misses)
		     : [amp] "r"(amp), [nops_per] "r"(nops_per), [hx] "r"(hx),
		       [hy] "r"(hy)
		     : "rcx", "rdx", "r8", "r9", "r10");

	*misses = __misses;
	*cycles = __cycles;
}

static void asm_measure_single_block(unsigned long hx, unsigned long hy,
				     size_t amp, size_t nops_per,
				     unsigned long *misses,
				     unsigned long *cycles)
{
	unsigned long __misses = 0;
	unsigned long __cycles = 0;

	assert(nops_per);

	asm volatile("mfence\n\t"
		     "lfence\n\t"
		     "mov $0x0, %%rcx\n\t"
		     "rdpmc\n\t"
		     "mov %%rax, %%r9\n\t"
		     "rdtsc\n\t"
		     "mov %%rax, %%r8\n\t"

		     /* NOPs */
		     "mov %[nops_per], %%rax\n\t"
		     "0:\n\t" SPLIT_DETECT_CONST_ASM "nop\n\t"
		     ".endr\n\t"
		     "dec %%rax\n\t"
		     "jnz 0b\n\t"

		     "mov %[amp], %%rax\n\t"
		     "1:\n\t"
		     ".rept 32\n\t"
		     "mov (%[hx]), %[hx]\n\t"
		     "mov (%[hy]), %[hy]\n\t"
		     ".endr\n\t"
		     "dec %%rax\n\t"
		     "jnz 1b\n\t"

		     "lfence\n\t" /* local instructions done */
		     "rdtsc\n\t"
		     "sub %%r8, %%rax\n\t"
		     "mov %%rax, %%rbx\n\t"
		     "mov $0x0, %%rcx\n\t"
		     "rdpmc\n\t"
		     "sub %%r9, %%rax\n\t"
		     : "+b"(__cycles), "+a"(__misses)
		     : [amp] "r"(amp), [nops_per] "r"(nops_per), [hx] "r"(hx),
		       [hy] "r"(hy)
		     : "rcx", "rdx", "r8", "r9", "r10");

	*misses = __misses;
	*cycles = __cycles;
}

#ifdef SLEDGEHAMMER
static void __hammer_sledge(struct pattern *p, size_t times, size_t banks)
{
	size_t size = p->blks[0].cs[0].size + p->blks[0].cs[1].size;
	assert(size % banks == 0);
	size_t sides = size / banks;

	for (size_t n = 0; n < times; n++) {
		for (size_t i = 0; i < sides; i++) {
			struct chase *c = &p->blks[0].cs[i % 2];
			for (size_t j = 0; j < banks; j++) {
				size_t k = (i / 2) * banks + j;
				assert(k < c->size);
				unsigned long addr = c->instance.addrs[k];
				*(volatile char *)addr;
			}
		}

		/* They say they have one here, in their paper */
		asm volatile("mfence\n\t");
	}
}

static void hammer_sledge(struct pattern *p, size_t banks)
{
	double factor = 1;
	size_t num_trefi_hammer = pattern_num_trefi_hammer(p->num_trefi_factor);
	size_t times = num_trefi_hammer / pattern_size_in_trefi(p);

	for (size_t i = 0; i < (1UL << p->num_tries_hammer); i++) {
		struct timespec start = { 0 };
		struct timespec stop = { 0 };

		clock_gettime(CLOCK_MONOTONIC_RAW, &start);

		__hammer_sledge(p, times, banks);

		clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

		double real =
			(double)((1000000000 * (stop.tv_sec - start.tv_sec)) +
				 (stop.tv_nsec - start.tv_nsec)) /
			1000;

		if (i == 0) {
			factor = real / (num_trefi_hammer * 7.8);
			times = times / factor;
		}

		printp("%02lu, %12.2f us, %02lu, %4.2fx\n", i, real,
		       p->num_blks, real / (num_trefi_hammer * 7.8));
	}
}
#endif

static void hammer_times(struct pattern *p, size_t times)
{
	if (p->trigger.enabled) {
		assert(p->multiblock.enabled);
		assert(p->multiblock.nops_per);

		for (size_t n = 0; n < times; n++) {
			for (size_t b = 0; b < p->num_blks; b++) {
				struct refblock *blk = p->blks[b].self;

				assert(blk->reps_of_blk == 1);

				unsigned long *xs = blk->cs[0].instance.addrs;
				unsigned long *ys = blk->cs[1].instance.addrs;

				if (b) {
					asm_hammer_evict_loop_single_block_remaining(
						xs[0], ys[0],
						blk->reps_per_ref);
				} else {
					asm_hammer_evict_loop_single_block_trigger(
						xs[0], ys[0],
						p->trigger.blk.cs[0]
							.instance.addrs[0],
						p->trigger.blk.cs[1]
							.instance.addrs[0],
						blk->reps_per_ref,
						SPLIT_DETECT_NUM_TRIGGERS_HAMMER,
						p->multiblock.nops_per);
				}
			}
		}
	} else if (p->multiblock.enabled) {
		assert(p->multiblock.nops_per);

		for (size_t n = 0; n < times; n++) {
			for (size_t b = 0; b < p->num_blks; b++) {
				struct refblock *blk = p->blks[b].self;

				assert(blk->reps_of_blk == 1);

				unsigned long *xs = blk->cs[0].instance.addrs;
				unsigned long *ys = blk->cs[1].instance.addrs;

				if (b) {
					if (p->rainbow) {
#ifdef RAINBOW2
						asm_hammer_evict_loop_single_block_remaining_rainbow2(
							xs[0], xs[1], ys[0],
							ys[1],
							blk->reps_per_ref);
#else
						asm_hammer_evict_loop_single_block_remaining_rainbow(
							xs[0], xs[1], xs[2],
							xs[3], ys[0], ys[1],
							ys[2], ys[3],
							blk->reps_per_ref);
#endif
					} else {
						asm_hammer_evict_loop_single_block_remaining(
							xs[0], ys[0],
							blk->reps_per_ref);
					}
				} else {
					if (p->rainbow) {
#ifdef RAINBOW2
						asm_hammer_evict_loop_single_block_rainbow2(
							xs[0], xs[1], ys[0],
							ys[1],
							blk->reps_per_ref,
							p->multiblock.nops_per);
#else
						asm_hammer_evict_loop_single_block_rainbow(
							xs[0], xs[1], xs[2],
							xs[3], ys[0], ys[1],
							ys[2], ys[3],
							blk->reps_per_ref,
							p->multiblock.nops_per);
#endif
					} else {
						asm_hammer_evict_loop_single_block(
							xs[0], ys[0],
							blk->reps_per_ref,
							p->multiblock.nops_per,
							0, true);
					}
				}
			}
		}
	} else { /* No trigger, no multiblock */
		for (size_t n = 0; n < times; n++) {
			for (size_t b = 0; b < p->num_blks; b++) {
				struct refblock *blk = p->blks[b].self;

				unsigned long *xs = blk->cs[0].instance.addrs;
				unsigned long *ys = blk->cs[1].instance.addrs;

				for (size_t r = 0; r < blk->reps_of_blk; r++) {
					if (p->rainbow) {
#ifdef RAINBOW2
						asm_hammer_evict_loop_single_block_rainbow2(
							xs[0], xs[1], ys[0],
							ys[1],
							blk->reps_per_ref,
							blk->nops_per_per_ref);
#else
						asm_hammer_evict_loop_single_block_rainbow(
							xs[0], xs[1], xs[2],
							xs[3], ys[0], ys[1],
							ys[2], ys[3],
							blk->reps_per_ref,
							blk->nops_per_per_ref);
#endif
					} else {
						asm_hammer_evict_loop_single_block(
							xs[0], ys[0],
							blk->reps_per_ref,
							blk->nops_per_per_ref,
							blk->__press_nops_per,
							p->interleave);
					}
				}
			}
		}
	}
}

static size_t pattern_verify_misses(struct pattern *p)
{
	double misses[PATTERN_NUM_TIMINGS] = { 0 };

	for (size_t i = 0; i < PATTERN_NUM_TIMINGS; i++) {
		double pre = __get_misses();
		hammer_times(p, 1);
		double post = __get_misses();
		misses[i] = post - pre;
	}

	return sort_print_quantiles_std_median(misses, PATTERN_NUM_TIMINGS);
}

static void argparse(int argc, char **argv, struct args *args)
{
	/*
	 * 0: pattern
	 *
	 * 1: Path to bank stats file
	 * 2: Path to slice info file
	 * 3: Path to pattern info file
	 * 4: Name of snapshot
	 */
	assert(argc == 5);

	args->bank_stats_file = argv[1];
	args->slice_info_file = argv[2];
	args->pat_info_file = argv[3];
	args->snapshot = argv[4];

	replay_info.snapshot = args->snapshot;

	printf("Will use bank stats file %s, slice info file %s, pat info file %s, snapshot is %s\n",
	       args->bank_stats_file, args->slice_info_file,
	       args->pat_info_file, args->snapshot);
}

enum addr_type { SAME_SET, SAME_SET_AND_BANK, SAME_SET_AND_ROW_PAIR };

/* Trailing zero bits */
static int tzb(size_t x)
{
	int y = 0;

	while (x) {
		if (x & 0x1UL) {
			return y;
		} else {
			y++;
			x = x >> 1;
		}
	}

	assert(0);
}

/* Leading zero bits */
static int lzb(size_t x)
{
	int y = 0;

	while (x) {
		if (x & 0x8000000000000000UL) {
			return y;
		} else {
			y++;
			x = x << 1;
		}
	}

	assert(0);
}

static struct eviction_set *new_eviction_set(size_t size)
{
	struct eviction_set *es = calloc(1, sizeof(struct eviction_set));
	assert(es);
	es->addrs = calloc(size, sizeof(unsigned long));
	assert(es->addrs);
	es->size = size;
	es->bank = -1;
	es->next = NULL;
	return es;
}

/* Only copies until whatever dest can hold */
static void copy_eviction_set(struct eviction_set *dest,
			      struct eviction_set *src)
{
	assert(dest->size == src->size);
	memcpy(dest->addrs, src->addrs, dest->size * sizeof(unsigned long));
}

static void free_chase_instance(struct chase *c)
{
	assert(c);

	if (c->instance.addrs) {
		free(c->instance.addrs);
		c->instance.addrs = NULL;
	}
}

static void new_chase_instance(struct chase *c, size_t size)
{
	assert(!c->instance.addrs);
	c->instance.addrs = calloc(size, sizeof(unsigned long));
	assert(c->instance.addrs);
}

static void free_eviction_set(struct eviction_set *es)
{
	assert(es);
	free(es->addrs);
	free(es);
}

static size_t set_is(unsigned long addr, struct llc_info *info)
{
	unsigned long mask = (1UL << info->num_set_bits) - 1;
	unsigned long set =
		((mask << info->num_line_bits) & addr) >> info->num_line_bits;

	return set;
}

static unsigned long set_becomes(unsigned long addr, unsigned long new_set,
				 struct llc_info *info)
{
	unsigned long max = 1UL << info->num_set_bits;
	unsigned long old_set = set_is(addr, info);

	assert(new_set < max);

	addr = addr ^ ((old_set ^ new_set) << info->num_line_bits);

	assert(set_is(addr, info) == new_set);

	return addr;
}

static unsigned long create_set_mask(struct llc_info *info)
{
	return ((1UL << info->num_set_bits) - 1) << info->num_line_bits;
}

static unsigned long create_column_mask(struct bank_stats *stats)
{
	unsigned long mask = 0;

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		if (stats->bank_functions[i]) {
			if (!(stats->bank_functions[i] & stats->row_mask)) {
				mask = ((1UL << tzb(stats->bank_functions[i])) ^
					(stats->bank_functions[i])) -
				       1;
				mask ^= 0x7; /* Remove bus offset */
				return mask;
			}
		}
	}

	assert(0);
}

static unsigned long create_bank_mask(struct llc_info *info,
				      struct bank_stats *stats,
				      bool allow_set_overlap)
{
	unsigned long mask = 0;

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		if (allow_set_overlap) {
			mask |= stats->bank_functions[i];
		} else if (!(stats->bank_functions[i] &
			     create_set_mask(info))) {
			mask |= stats->bank_functions[i];
		}
	}

	return mask;
}

static int ____log2(size_t x)
{
	int y = 0;

	while (x) {
		x = x >> 1;
		y++;
	}

	return y;
}

/* Nonzero bits */
static int nzb(size_t x)
{
	int y = 0;

	while (x) {
		if (x & 0x1) {
			y++;
		}

		x = x >> 1;
	}

	return y;
}

#ifdef KABY_LAKE
static size_t kaby_slice_is(unsigned long addr)
{
	size_t slice = 0;

	for (size_t i = 0; i < __KABY_SLICE_MASKS_SIZE; i++) {
		size_t sub = nzb(addr & __KABY_SLICE_MASKS[i]) % 2;
		slice = slice ^ (sub << i);
	};

	return slice;
};
#endif

static unsigned long bank_become_keep_row(unsigned long addr,
					  unsigned long new_bank,
					  struct bank_stats *stats)
{
	unsigned long old_bank = bank_is(addr, stats->bank_functions);

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		if (((old_bank >> i) & 0x1UL) != ((new_bank >> i) & 0x1UL)) {
			unsigned long bank_function = stats->bank_functions[i];

			assert(bank_function);

			if ((bank_function & stats->row_mask) ==
			    bank_function) {
				assert(0);
			} else if (bank_function & stats->row_mask) {
				/*
				 * Choose the one that does not overlap
				 */
				addr ^= (~stats->row_mask & bank_function);
			} else {
				/*
				 * Neither of them overlap: choose one.  We
				 * currently choose the lowest
				 */
				addr ^= (1UL << tzb(bank_function));
			}
		}
	}

	assert(bank_is(addr, stats->bank_functions) == new_bank);

	return addr;
}

static size_t __rainbow_to_par(bool rainbow)
{
#ifdef RAINBOW2
	return rainbow ? 2 : 1;
#else
	return rainbow ? 4 : 1;
#endif
}

/* @par: how many subchases... in parallel: 1 or 8? */
static void __install_chase(unsigned long *xs, size_t n, size_t par)
{
	assert(n % par == 0);

	for (size_t i = 0; i < n; i += par) {
		for (size_t k = 0; k < par; k++) {
			size_t src = i + k;
			size_t dest = (i + k + par) % n;

			assert(xs[src]);

			*(unsigned long *)xs[src] = xs[dest];
		}
	}
}

static void refblock_install(struct refblock *blk)
{
	for (size_t i = 0; i < PATTERN_NUM_CHASES; i++) {
		struct chase *c = &blk->cs[i];
		assert(c->instance.addrs);
		__install_chase(c->instance.addrs, c->size,
				__rainbow_to_par(c->rainbow));
	}
}

static unsigned long bank_try_become_keep_set(unsigned long addr,
					      unsigned long new_bank,
					      struct llc_info *info,
					      struct bank_stats *stats)
{
	unsigned long set_mask = create_set_mask(info);
	unsigned long old_bank = bank_is(addr, stats->bank_functions);

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		if (((old_bank >> i) & 0x1UL) != ((new_bank >> i) & 0x1UL)) {
			unsigned long bank_function = stats->bank_functions[i];

			assert(bank_function);

			if ((bank_function & set_mask) == bank_function) {
				/* Impossible! */
				return 0;
			} else if (bank_function & set_mask) {
				/*
				 * Choose the one that does not overlap
				 */
				addr ^= (~set_mask & bank_function);
			} else {
				/*
				 * Neither of them overlap: choose one.  We
				 * currently choose the lowest
				 */
				addr ^= (1UL << tzb(bank_function));
			}
		}
	}

	assert(bank_is(addr, stats->bank_functions) == new_bank);

	return addr;
}

static unsigned long row_is(unsigned long addr, unsigned long row_mask)
{
	return (addr & row_mask) >> tzb(row_mask);
}

static unsigned long column_is(unsigned long addr, unsigned long column_mask)
{
	return row_is(addr, column_mask);
}

static unsigned long row_becomes(unsigned long addr, unsigned long new_row,
				 unsigned long row_mask)
{
	unsigned long max = 1UL << nzb(row_mask);
	unsigned long old_row = row_is(addr, row_mask);

	assert(new_row < max);

	addr = addr ^ ((old_row ^ new_row) << tzb(row_mask));

	assert(row_is(addr, row_mask) == new_row);

	return addr;
}

static unsigned long column_becomes_keep_bank(unsigned long addr,
					      unsigned long new_column,
					      unsigned long column_mask,
					      struct bank_stats *stats)
{
	unsigned long old_bank = bank_is(addr, stats->bank_functions);
	unsigned long old_column = column_is(addr, column_mask);
	unsigned long mask = (old_column ^ new_column) << tzb(column_mask);

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		unsigned long overlap = stats->bank_functions[i] & mask;

		if (overlap) {
			assert(nzb(overlap) == 1);
			addr = addr ^ (stats->bank_functions[i] ^ overlap);
			break;
		}
	}

	addr = row_becomes(addr, new_column, column_mask);

	assert(column_is(addr, column_mask) == new_column);
	assert(bank_is(addr, stats->bank_functions) == old_bank);

	return addr;
}

static int second_lsb_row_mask(unsigned long row_mask)
{
	return tzb(row_mask) + 1;
}

static void single_chase(unsigned long *a, size_t num_iters, size_t amp,
			 double *misses, double *cycles)
{
	double timings[PATTERN_NUM_TIMINGS_SINGLE] = { 0 };
	double counts[PATTERN_NUM_TIMINGS_SINGLE] = { 0 };

	__install_chase(a, num_iters, 1);

	for (size_t i = 0; i < PATTERN_NUM_TIMINGS_SINGLE; i++) {
		unsigned long t = 0;
		unsigned long c = 0;
		single_chase_into_t(a[0], t, c, amp * num_iters);
		timings[i] = t;
		counts[i] = (double)c / amp;
	}

	if (misses) {
		*misses = gsl_stats_median(counts + PATTERN_WARMUP_ROUNDS, 1,
					   PATTERN_NUM_TIMINGS_SINGLE -
						   PATTERN_WARMUP_ROUNDS);
	}

	if (cycles) {
		*cycles = gsl_stats_min(timings + PATTERN_WARMUP_ROUNDS, 1,
					PATTERN_NUM_TIMINGS_SINGLE -
						PATTERN_WARMUP_ROUNDS);
	}
}

/*
 * static double quantiles[] = { QUANTILES_STD };
 * double values[QUANTILES_STD_NUM] = { 0 };
 *
 * sort_print_quantiles(timings, PATTERN_NUM_TIMINGS, quantiles, QUANTILES_STD_NUM,
 * values);
 *
 * NOTE: not used!
 */
static void double_chase(unsigned long *a, unsigned long *b, size_t num_iters,
			 size_t amp, double *misses, double *cycles)
{
	double timings[PATTERN_NUM_TIMINGS] = { 0 };
	double counts[PATTERN_NUM_TIMINGS] = { 0 };

	__install_chase(a, num_iters, 1);
	__install_chase(b, num_iters, 1);

	for (size_t i = 0; i < PATTERN_NUM_TIMINGS; i++) {
		unsigned long t = 0;
		unsigned long c = 0;
		unsigned long n = amp * num_iters;

		asm volatile(
			"mfence\n\t" /* globally visible loads and stores */
			"lfence\n\t" /* mfence and anything else done */
			"mov $0x0, %%rcx\n\t"
			"rdpmc\n\t"
			"mov %%rax, %%r9\n\t"
			"rdtsc\n\t"
			"mov %%rax, %%r8\n\t"
			"lfence\n\t" /* rdtsc/rdpmc done */

			"mov %4, %%r10\n\t" /* Copy loop iter. */

			"1:\n\t"
			"mov (%2), %2\n\t"
			"mov (%3), %3\n\t"
			"dec %%r10\n\t"
			"jnz 1b\n\t"

			"mfence\n\t" /* globally visible loads and stores */
			"lfence\n\t" /* local instructions done */
			"rdtsc\n\t"
			"sub %%r8, %%rax\n\t"
			"mov %%rax, %%rbx\n\t"
			"mov $0x0, %%rcx\n\t"
			"rdpmc\n\t"
			"sub %%r9, %%rax\n\t"
			: "+b"(t), "+a"(c), "+r"(a[0]), "+r"(b[0])
			: "r"(n)
			: "rcx", "rdx", "r8", "r9", "r10", "memory");

		timings[i] = t;
		counts[i] = (double)c / amp;
	}

	if (misses) {
		*misses = gsl_stats_median(counts + PATTERN_WARMUP_ROUNDS, 1,
					   PATTERN_NUM_TIMINGS -
						   PATTERN_WARMUP_ROUNDS);
	}

	if (cycles) {
		*cycles = gsl_stats_min(timings + PATTERN_WARMUP_ROUNDS, 1,
					PATTERN_NUM_TIMINGS -
						PATTERN_WARMUP_ROUNDS);
	}
}

static size_t single_chase_filter(unsigned long *candidates,
				  size_t num_candidates, struct llc_info *info)
{
	for (size_t n = num_candidates; n >= info->wayness + 1; n--) {
		double misses = 0;

		single_chase(&candidates[0], n, PATTERN_CHASE_AMP, &misses,
			     NULL);

		if (misses <= PATTERN_SINGLE_CHASE_MIN_MISSES) {
			/*
			 * n = i + 1: slow (old)
			 * n = i: fast (new)
			 *
			 * -> candidate @i is good. Remove a
			 *  random candidate from the last i
			 *  candidates.
			 *
			 *  old was slower, newer should also
			 *  be slow
			 */
			double kisses = 0;

			/* May fail early due to random addresses: restart */
			assert(n < num_candidates);

			for (size_t k = 0; k < n * PATTERN_NUM_TRIES; k++) {
				size_t j = k % n;

				unsigned long tmp = candidates[j];

				candidates[j] = candidates[n];

				assert(candidates[j]);

				single_chase(&candidates[0], n,
					     PATTERN_CHASE_AMP, &kisses, NULL);

				printp("popping %lu... (%.2f)\n", j, kisses);

				if (kisses > PATTERN_SINGLE_CHASE_MIN_MISSES) {
					misses = kisses;
					break;
				}

				/* Undo */
				candidates[j] = tmp;
			}

			/*
			 * Loop done: either we found
			 * something, or the loop just ended...
			 */
			if (misses != kisses) {
				if ((n + 1) == (info->wayness + 1)) {
					/* Dead code? */
					assert(0);
					return 1;
				} else {
					/* Start over */
					assert(0);
				}
			}
		}

		/* Difference below threshold: tolerated */
		printf("%lu: %.2f\n", n, misses);
	}

	return 1;
}

/*
 * static double random_chase(unsigned long base, size_t size, size_t num)
 * {
 *         unsigned long *tmp = calloc(num, sizeof(unsigned long));
 *
 *         assert(tmp);
 *
 *         for (size_t i = 0; i < num; i++) {
 *                 tmp[i] = base + rand() % size;
 *         }
 *
 *         double avg = 0;
 *
 *         single_chase(tmp, num, PATTERN_CHASE_AMP, NULL, &avg);
 *
 *         free(tmp);
 *
 *         return avg;
 * }
 */

static struct eviction_set *
new_root_eviction_set(unsigned long in, unsigned long mask_available_bits,
		      struct llc_info *info)
{
	/*
	 * For simplicity, we assume all bits contribute to the slice.
	 * For the set index we typically need 10 bits
	 *
	 * We do this once and get 2 * 1024 eviction sets!
	 */
	unsigned long mask =
		mask_available_bits ^
		((1UL << (info->num_set_bits + info->num_line_bits)) - 1);

	/*
	 * This is a variant of the birthday problem, see:
	 * https://www.wolframalpha.com/input?i=plot+line+y+%3D+1+-+e%5E%28-%28N+choose+17%29%2F12%5E16%29+for+N+from+0+to+100
	 *
	 * We could do: num_candidates = info->num_cpu_cores * info->wayness +
	 * 1 to have a guarantee, but then we usually find two sets, which is a
	 * bit annoying, and takes longer.
	 */
	size_t num_candidates = 132;

	assert(nzb(mask) + 1 >= ____log2(num_candidates));

	unsigned long *candidates =
		calloc(num_candidates, sizeof(unsigned long));

	assert(candidates);

	/*
	 * We'd just add the numbers 0-132 to our base here? Added some
	 * randomness
	 */
	for (size_t i = 0; i < num_candidates; i++) {
		size_t upper_bits_mask = rand() % (1UL << nzb(mask));
		candidates[i] = in ^ (upper_bits_mask << tzb(mask));
		assert(candidates[i] >= in);
		assert(candidates[i] < in + (1UL << 30));
	};

	assert(single_chase_filter(candidates, num_candidates, info));

	struct eviction_set *es = new_eviction_set(info->wayness + 1);

	memcpy(es->addrs, candidates, es->size * sizeof(unsigned long));

	free(candidates);

	return es;
}

/*
 * static void print_dense_loop(struct dense_loop *dl, struct llc_info *info,
 *                              struct bank_stats *stats)
 * {
 *         printp("dense loop %p\n", dl);
 *         printp("index,addr,bank,row,set,h/m?\n");
 *         for (size_t i = 0; i < dl->size; i++) {
 *                 printp("%02lu,0x%016lx,%2u,%4lu,%4lu,%s\n", i, dl->addrs[i],
 *                        bank_is(dl->addrs[i], stats->bank_functions),
 *                        row_is(dl->addrs[i], stats->row_mask),
 *                        set_is(dl->addrs[i], info),
 *                        dl->miss_at_index[i] ? "m" : "");
 *         }
 * }
 */

static void print_eviction_set(struct eviction_set *es, struct llc_info *info,
			       struct bank_stats *stats)
{
	printp("eviction set %p\n", es);
#ifdef KABY_LAKE
	printp(" i,          addr,bk, row,set\n");
#else
	printp(" i,          addr,bk, row,set,slice\n");
#endif
	for (size_t i = 0; i < es->size; i++) {
#ifdef KABY_LAKE
		printp("%02lu,0x%12lx,%02u,%04lu,%03lu,%01lu\n", i,
		       es->addrs[i],
		       bank_is(es->addrs[i], stats->bank_functions),
		       row_is(es->addrs[i], stats->row_mask),
		       set_is(es->addrs[i], info), kaby_slice_is(es->addrs[i]));
#else
		printp("%02lu,0x%12lx,%02u,%04lu,%03lu\n", i, es->addrs[i],
		       bank_is(es->addrs[i], stats->bank_functions),
		       row_is(es->addrs[i], stats->row_mask),
		       set_is(es->addrs[i], info));
#endif
	}
}

static void print_double_chase(struct refblock *blk, struct llc_info *info,
			       struct bank_stats *stats)
{
	assert(PATTERN_NUM_CHASES == 2);

	struct chase *c = &blk->cs[0];
	struct chase *other = &blk->cs[1];

	printp("instances %p-%p (gather? %d)\n", c, other, c->gather);
	printp(" i,           addr,bk, row,set |            addr,bk, row,set\n");

	for (size_t i = 0; i < c->size; i++) {
		for (size_t j = 0; j < PATTERN_NUM_CHASES; j++) {
			unsigned long *addrs = blk->cs[j].instance.addrs;
			bool *miss_at_index = blk->cs[j].miss_at_index;
			size_t *flips_around_index =
				blk->cs[j].flips_around_index;

			if (j == 0) {
				printp("%02lu,%s0x%12lx%s,%02u,%04lu,%03lu %04lu |",
				       i, miss_at_index[i] ? "" : " ", addrs[i],
				       miss_at_index[i] ? " " : "",
				       bank_is(addrs[i], stats->bank_functions),
				       row_is(addrs[i], stats->row_mask),
				       info ? set_is(addrs[i], info) : 0,
				       flips_around_index[i]);
			} else {
				printf(" %s0x%12lx%s,%02u,%04lu,%03lu %04lu\n",
				       miss_at_index[i] ? "" : " ", addrs[i],
				       miss_at_index[i] ? " " : "",
				       bank_is(addrs[i], stats->bank_functions),
				       row_is(addrs[i], stats->row_mask),
				       info ? set_is(addrs[i], info) : 0,
				       flips_around_index[i]);
			}
		}

		if ((i + 1) % info->wayness == 0) {
			printf("\n");
		}
	}
}

/*
 * If this function is applied, you add/subtract 2 to the row and preserve bank
 */
static unsigned long find_double_sided_bank_function(struct bank_stats *stats)
{
	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		unsigned long mask = stats->bank_functions[i] & stats->row_mask;
		if ((mask >> tzb(stats->row_mask)) == 0x2)
			return stats->bank_functions[i];
	}

	return 0;
}

static unsigned long
find_set_overlapping_bank_function(struct llc_info *info,
				   struct bank_stats *stats)
{
	unsigned long sm = create_set_mask(info);

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		unsigned long bf = stats->bank_functions[i];

		if ((sm & bf) == bf) {
			return bf;
		}
	}

	return 0;
}

static size_t find_permus_of_other_bank_functions(struct llc_info *info,
						  struct bank_stats *stats,
						  unsigned long *masks,
						  size_t max)
{
	unsigned long ds = find_double_sided_bank_function(stats);
	unsigned long so = find_set_overlapping_bank_function(info, stats);

	size_t k = 0;

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		unsigned long bf = stats->bank_functions[i];

		if (bf && (bf != ds) && (bf != so)) {
			size_t tmp = k;

			assert(k < max);
			masks[k++] = bf;

			for (size_t j = 0; j < tmp; j++) {
				assert(k < max);
				masks[k++] = masks[j] ^ masks[tmp];
			}
		}
	}

	return k;
}

static size_t count_banks(struct bank_stats *stats)
{
	size_t n = 0;

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		if (stats->bank_functions[i]) {
			/* printf("%lu: 0x%lx\n", i, stats->bank_functions[i]); */
			n++;
		}
	}

	return (1UL << n);
}

static struct eviction_set *addr_to_sledge_set(unsigned long base, size_t sides,
					       size_t banks,
					       struct bank_stats *stats)
{
#define SLEDGEHAMMER_MAX_SIDES 20
#define SLEDGEHAMMER_NUM_BANKS 6
	unsigned long row_mask = stats->row_mask;
	size_t num_rows = 1UL << nzb(row_mask);
	size_t num_banks = count_banks(stats);

	struct eviction_set *new = new_eviction_set(sides * banks);

	for (size_t i = 0; i < new->size; i++) {
		new->addrs[i] = base;
	}

	for (size_t i = 0; i < new->size; i += banks) {
		unsigned long row =
			(row_is(new->addrs[i - !!i * banks], row_mask) +
			 2 * !!i) %
			num_rows;

		for (size_t j = 0; j < banks; j++) {
			size_t k = i + j;

			if (k == 0) continue;

			unsigned long bank;

			if (j == 0) {
				assert(i);
				bank = bank_is(new->addrs[k - banks],
					       stats->bank_functions);
			} else {
				bank = (bank_is(new->addrs[k - 1],
						stats->bank_functions) +
					1) %
				       num_banks;
			}

			new->addrs[k] =
				row_becomes(new->addrs[k], row, row_mask);
			new->addrs[k] = bank_become_keep_row(new->addrs[k],
							     bank, stats);
		}
	}

	return new;
}

static struct eviction_set *
fork_eviction_set_with_new_set(struct eviction_set *old, size_t add_to_set,
			       struct llc_info *info)
{
	struct eviction_set *new = new_eviction_set(old->size);

	copy_eviction_set(new, old);

	for (size_t i = 0; i < new->size; i++) {
		unsigned long old_set = set_is(new->addrs[i], info);
		unsigned long new_set =
			(old_set + add_to_set) % (1UL << info->num_set_bits);

		new->addrs[i] = set_becomes(new->addrs[i], new_set, info);

		if (i) {
			assert(set_is(old->addrs[i], info) ==
			       set_is(old->addrs[i - 1], info));
			assert(set_is(new->addrs[i], info) ==
			       set_is(new->addrs[i - 1], info));
		}
	}

	return new;
}

/*
 * static struct eviction_set *merge_eviction_sets(struct eviction_set *one,
 *                                                 struct eviction_set *two)
 * {
 *         assert(one->size == two->size);
 *
 *         struct eviction_set *new = new_eviction_set(one->size + two->size);
 *
 *         for (size_t i = 0; i < new->size; i += 2) {
 *                 new->addrs[i] = one->addrs[i / 2];
 *                 new->addrs[i + 1] = two->addrs[i / 2];
 *         }
 *
 *         return new;
 * }
 */

static size_t non_uniformity_to_lanes(size_t non_uniformity)
{
	return 2 + (2 * non_uniformity);
}

/*
 * Given a lane index @m, which "eviction set lane" (A, B, or C) should we use?
 *
 * Remember, these are the options:
 *
 * A, B
 * A, B, A, C
 * A, B, A, B, A, C
 * A, B, A, B, A, B, A, C
 */
static size_t __transform_m(size_t nu, size_t m)
{
	size_t lanes = non_uniformity_to_lanes(nu);

	assert(m < lanes);

	if (nu == 0) {
		return m;
	} else if (m + 1 == lanes) {
		return 2;
	} else {
		return m % 2;
	}
}

static void sledge_set_to_pattern(struct pattern *p, unsigned long base,
				  size_t sides, size_t banks,
				  struct bank_stats *stats)
{
	size_t first = sides / 2 + (sides % 2);
	struct eviction_set *es = addr_to_sledge_set(base, sides, banks, stats);
	assert(sides * banks <= es->size);

	for (size_t i = 0; i < PATTERN_NUM_CHASES; i++) {
		struct chase *c = &p->blks[0].cs[i];
		free_chase_instance(c);

		c->hit_reps = 0;
		c->size = (i == 0 ? first : sides - first) * banks;
		c->misses_per_set = c->size;
		c->miss_at_index = calloc(c->size, sizeof(size_t));

		if (c->flips_around_index) free(c->flips_around_index);

		c->flips_around_index = calloc(c->size, sizeof(size_t));
		assert(c->flips_around_index);

		new_chase_instance(c, c->size);
		assert(c->size % banks == 0);

		for (size_t j = 0; j < c->size; j += banks) {
			for (size_t k = 0; k < banks; k++) {
				size_t l =
					PATTERN_NUM_CHASES * j + i * banks + k;

				c->instance.addrs[j + k] = es->addrs[l];
				c->miss_at_index[j + k] = true;
			}
		}
	}
}

static void chase_instance_from_eviction_set(struct eviction_set *es,
					     struct chase *c,
					     struct llc_info *info,
					     struct bank_stats *stats)
{
	assert(c->hit_reps == 1 && c->hit_reps <= 4);

	/* In theory, it can be <= info->wayness */
	assert(c->misses_per_set <= PATTERN_CHASE_HIGH_DENSITY);

	/* TODO: fix this __WAYNESS shit */
	assert(c->size == non_uniformity_to_lanes(c->non_uniformity) *
				  c->hit_reps * info->wayness);
	new_chase_instance(c, c->size);

	assert(es->size >= info->wayness + c->misses_per_set);

	unsigned long *addrs = c->instance.addrs;
	bool *miss_at_index = NULL;

	if (c->miss_at_index) {
		miss_at_index = c->miss_at_index;
	} else {
		miss_at_index = calloc(c->size, sizeof(bool));
	}

	assert(miss_at_index);

	for (size_t i = 0; i < c->size; i++) {
		size_t j = i % info->wayness;
		size_t m = __transform_m(c->non_uniformity,
					 i / (info->wayness * c->hit_reps));
		size_t shift = i / info->wayness;

		assert(shift < (info->line_size / sizeof(unsigned long)));

		if (j < c->misses_per_set) {
			/* Miss */
			assert(m < PATTERN_NUM_LANES_PER_EVICTION_SET);

			addrs[i] = es->addrs[m * info->wayness + j] +
				   shift * sizeof(unsigned long);
			miss_at_index[i] = true;

			/* 
			 * Old: this was used to support hit_reps > 1
			 *
			 * if (!(shift == 0 || shift == c->hit_reps)) {
			 * assert(shift > 0); size_t num_sets = 1UL <<
			 * info->num_set_bits; unsigned long old =
			 * set_is(addrs[i], info);
			 *
			 * Adjacent cache line prefetcher!
			 *
			 * unsigned long new = (old + num_sets / (shift + 1)) %
			 * num_sets; addrs[i] = set_becomes(addrs[i], new,
			 * info); miss_at_index[i] = false;
			 *
			 */
		} else {
			/* Hit */
			addrs[i] = es->addrs[j] + shift * sizeof(unsigned long);
			miss_at_index[i] = false;
		}
	}

	/* Reorder so as to minimize the gaps */
	size_t scatter[] = {
		0x0, 0x1, 0x101, 0x841, 0x1111, 0x2491, 0x2929, 0x5255, 0x5555,
	};

	size_t gather[] = {
		0x0, 0x1, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff,
	};

	size_t *dist_masks = c->gather ? gather : scatter;
	size_t dist_mask = 0;
	size_t lane_size = info->wayness * c->hit_reps;

	assert(lane_size == 16);

	if (c->misses_per_set <= 8) {
		dist_mask = dist_masks[c->misses_per_set];
	} else {
		dist_mask = ~dist_masks[2 * 8 - c->misses_per_set] & 0xffff;

		unsigned long tmp = 0;

		/* Reverse */
		for (ssize_t i = lane_size - 1; i >= 0; i--) {
			tmp >>= 1;

			unsigned long x = !!(dist_mask & (1UL << i));
			assert(x == 0x0 || x == 0x1);
			tmp = tmp ^ (x << (lane_size - 1));
		}

		dist_mask = tmp;
	}

	assert(nzb(dist_mask) == (int)c->misses_per_set);

	size_t next_hit_at = c->misses_per_set;

	for (size_t i = next_hit_at; i < lane_size; i++) {
		assert(!miss_at_index[i]);
		assert(!miss_at_index[i + lane_size]);
	}

	for (size_t i = 0; i < lane_size; i++) {
		/* Should be a hit, but is a miss */
		if (!(dist_mask & (1UL << i)) && miss_at_index[i]) {
			assert(miss_at_index[i + lane_size]);

			for (size_t j = 0;
			     j < non_uniformity_to_lanes(c->non_uniformity);
			     j++) {
				assert(next_hit_at < lane_size);

				size_t hit = j * lane_size + next_hit_at;
				size_t miss = j * lane_size + i;

				unsigned long tmp_addr = addrs[hit];
				addrs[hit] = addrs[miss];
				addrs[miss] = tmp_addr;

				bool tmp_bool = miss_at_index[hit];

				/* Should be a hit there! */
				assert(!tmp_bool);

				miss_at_index[hit] = miss_at_index[miss];
				miss_at_index[miss] = tmp_bool;

				/* And now a miss at the old position */
				assert(miss_at_index[hit]);
			}

			next_hit_at++;
		}

		assert(!!(dist_mask & (1UL << i)) == miss_at_index[i]);
		assert(!!(dist_mask & (1UL << i)) ==
		       miss_at_index[i + lane_size]);
	}

	c->instance.bank = es->bank;

	if (!c->miss_at_index) free(miss_at_index);
}

/*
 * Swaps the address at index with the one at 0 and checks if eviction still
 * works
 */
static bool fast_chase_swap(struct eviction_set *es, size_t index,
			    struct llc_info *info, struct bank_stats *stats,
			    double expected_misses, double *misses_if_fail,
			    double *cycles_if_success)
{
	size_t min_size_for_chase = info->wayness + 1;

	assert(es->size >= min_size_for_chase);
	assert(index < es->size);

	struct eviction_set *fs = new_eviction_set(min_size_for_chase);

	memcpy(fs->addrs, es->addrs,
	       min_size_for_chase * sizeof(unsigned long));

	unsigned long tmp = fs->addrs[0];
	fs->addrs[0] = es->addrs[index];

	if (index < min_size_for_chase) {
		fs->addrs[index] = tmp;
	}

	struct chase chase = { .size = 2 * 1 * __WAYNESS,
			       .misses_per_set = 1,
			       .hit_reps = 1,
			       .gather = false };

	chase_instance_from_eviction_set(fs, &chase, info, stats);

	double misses;
	double cycles;

	single_chase(chase.instance.addrs, chase.size,
		     PATTERN_CHASE_AMP_FAST_CHASE, &misses, &cycles);

	free_chase_instance(&chase);

	if (misses < expected_misses) {
	__fast_chase_swap_fail:
		free_eviction_set(fs);
		if (misses_if_fail) *misses_if_fail = misses;
		return false;
	} else if (misses > expected_misses) {
		/* Only OK if Kaby Lake */
#ifdef KABY_LAKE
		size_t set = set_is(fs->addrs[0], info);
		size_t slice = kaby_slice_is(fs->addrs[0]);

		for (size_t i = 1; i < min_size_for_chase; i++) {
			if (set_is(fs->addrs[i], info) != set) {
				goto __fast_chase_swap_fail;
			}

			if (kaby_slice_is(fs->addrs[i]) != slice) {
				goto __fast_chase_swap_fail;
			}
		}

		/* Falls through */
#else
		goto __fast_chase_swap_fail;
#endif
	} else {
		/* All good */
		assert(misses == expected_misses);
	}

	if (cycles_if_success) *cycles_if_success = cycles;
	return true;
}

static bool fast_chase(struct eviction_set *es, struct llc_info *info,
		       struct bank_stats *stats, double expected_misses,
		       double *misses_if_fail, double *min_cycles_if_success)
{
	size_t min_size_for_chase = info->wayness + 1;

	assert(es->size >= min_size_for_chase);

	/* (We're shifting!) */
	size_t num_chases = es->size - min_size_for_chase + 1;

	/* +1 because if we have exactly min_size_for_dense_loop we do one */
	double *cycles = calloc(num_chases, sizeof(double));

	assert(cycles);

	/* The chase is two lanes, for which we need wayness + 1 addresses */
	struct chase chase = { .size = 2 * 1 * __WAYNESS,
			       .misses_per_set = 1,
			       .hit_reps = 1,
			       .gather = false };

	for (size_t shift = 0; shift < num_chases; shift++) {
		double misses;

		struct eviction_set *fs = new_eviction_set(min_size_for_chase);

		memcpy(fs->addrs, es->addrs + shift,
		       min_size_for_chase * sizeof(unsigned long));

		chase_instance_from_eviction_set(fs, &chase, info, stats);

		single_chase(chase.instance.addrs, chase.size,
			     PATTERN_CHASE_AMP_FAST_CHASE, &misses,
			     &cycles[shift]);

		free_chase_instance(&chase);

		if (misses < expected_misses) {
		__fast_chase_fail:
			free_eviction_set(fs);
			if (misses_if_fail) *misses_if_fail = misses;
			return false;
		} else if (misses > expected_misses) {
			/* Only OK if Kaby Lake */
#ifdef KABY_LAKE
			size_t set = set_is(fs->addrs[0], info);
			size_t slice = kaby_slice_is(fs->addrs[0]);

			for (size_t i = 1; i < min_size_for_chase; i++) {
				if (set_is(fs->addrs[i], info) != set) {
					goto __fast_chase_fail;
				}

				if (kaby_slice_is(fs->addrs[i]) != slice) {
					goto __fast_chase_fail;
				}
			}
#else
			goto __fast_chase_fail;
#endif
		} else {
			/* All good */
			assert(misses == expected_misses);
		}
	}

	if (min_cycles_if_success) {
		*min_cycles_if_success = gsl_stats_min(cycles, 1, num_chases);
	}

	return true;
}

static void eviction_set_reorder(struct eviction_set *es, size_t num_misses_per,
				 struct llc_info *info,
				 struct bank_stats *stats)
{
	assert(num_misses_per <= PATTERN_CHASE_HIGH_DENSITY);
	assert(es->bank >= 0);

	/* NOTE: elegant way to support multi-lane! */
	for (size_t i = 0; i < es->size; i++) {
		size_t j = i % info->wayness;

		if (j < num_misses_per) {
			long b = bank_is(es->addrs[i], stats->bank_functions);

			if (b != es->bank) {
				for (ssize_t k = es->size - 1;
				     k >= (ssize_t)num_misses_per; k--) {
					long c = bank_is(es->addrs[k],
							 stats->bank_functions);

					/* Swap */
					if (c == es->bank) {
						unsigned long tmp =
							es->addrs[i];
						es->addrs[i] = es->addrs[k];
						es->addrs[k] = tmp;
						goto next;
					}
				}

				printp("failed to reorder for %lu misses:\n",
				       num_misses_per);
				print_eviction_set(es, info, stats);
				assert(0);
			next:;
			}
		}
	}
}

static bool chase_has_row(struct chase *c, size_t bank, size_t row,
			  struct bank_stats *stats)
{
	for (size_t i = 0; i < c->size; i++) {
		if (bank !=
		    bank_is(c->instance.addrs[i], stats->bank_functions)) {
			continue;
		}

		if (row_is(c->instance.addrs[i], stats->row_mask) == row) {
			return true;
		}
	}

	return false;
}

static int is_in_br(struct br *list, size_t size, struct br element)
{
	for (size_t i = 0; i < size; i++) {
		if (list[i].b == element.b && list[i].r == element.r) return 1;
	}

	return 0;
}

static int is_in(size_t *list, size_t size, size_t element)
{
	for (size_t i = 0; i < size; i++) {
		if (list[i] == element) return 1;
	}

	return 0;
}

static int is_in_but_not(size_t *list, size_t size, size_t index)
{
	for (size_t i = 0; i < size; i++) {
		if (i == index) continue;
		if (list[i] == list[index]) return i;
	}

	return -1;
}

static struct eviction_set *
eviction_set_sweep_set_bits(struct eviction_set *es,
			    unsigned long available_mask_bits,
			    struct llc_info *info, struct bank_stats *stats)
{
	/*
	 * NOTE: assumes that there are no gaps in the mask, so no bank bit 7
	 * for example!
	 */
	unsigned long mask = create_set_mask(info);
	unsigned long extra = find_set_overlapping_bank_function(info, stats);

	mask = mask & available_mask_bits;
	mask = ~create_bank_mask(info, stats, true) & mask;

	size_t num_xors = 1UL << nzb(mask);
	size_t start = tzb(mask);

	assert(es);

	struct eviction_set *copy = new_eviction_set(es->size);

	copy_eviction_set(copy, es);

	for (size_t i = 0; i < num_xors; i++) {
		for (size_t j = 0; j < 2; j++) {
			unsigned long mask = (i << start) ^ (j * extra);
			/*
			 * unsigned long new_set =
			 *         set_is(es->addrs[0] ^ mask, info);
			 */

			/*
			 * NOTE: for some DIMMs, e.g. A6, this prevents us from
			 * finding the counter set...
			 *
			 * if (!is_in(fixed_policy_sets,
			 *            PATTERN_NUM_FIXED_POLICY_SETS, new_set)) {
			 *         continue;
			 * } else {
			 *         printf("0x%lx: got set %lu\n", copy, new_set);
			 * }
			 */

			for (size_t k = 0; k < es->size; k++) {
				copy->addrs[k] = es->addrs[k] ^ mask;
			}

			if (fast_chase(copy, info, stats,
				       PATTERN_FAST_CHASE_MISSES, NULL, NULL)) {
				return copy;
			}
		}
	}

	free_eviction_set(copy);
	return NULL;
}

/* TODO: rename, something like sweep_nonbank_row_bits? */
static struct eviction_set *
____eviction_set_sweep_row_bits(struct eviction_set *es,
				unsigned long available_mask_bits,
				struct llc_info *info, struct bank_stats *stats)
{
	size_t start =
		tzb(stats->row_mask & ~create_bank_mask(info, stats, true));
	size_t stop = tzb(available_mask_bits + 1);
	size_t available_row_mask = ((1UL << (stop - start)) - 1) << start;
	size_t num_xors = (available_row_mask + 1) >> start;

	struct eviction_set *copy = new_eviction_set(es->size);
	copy_eviction_set(copy, es);

	for (size_t i = 1; i < num_xors; i++) {
		for (size_t j = 0; j < es->size; j++) {
			copy->addrs[j] = es->addrs[j] ^ (i << start);
		}

		if (fast_chase(copy, info, stats, PATTERN_FAST_CHASE_MISSES,
			       NULL, NULL)) {
			printp("got >= %.2f misses for mask 0x%lx\n",
			       PATTERN_FAST_CHASE_MISSES, i << start);
			return copy;
		}
	}

	free_eviction_set(copy);
	return NULL;
}

static struct eviction_set *
eviction_set_sweep_row_bits(struct eviction_set *es,
			    unsigned long available_mask_bits,
			    struct llc_info *info, struct bank_stats *stats)
{
	size_t start = tzb(stats->row_mask);
	size_t stop = tzb(available_mask_bits + 1);
	size_t available_row_mask = ((1UL << (stop - start)) - 1) << start;
	size_t num_xors = (available_row_mask + 1) >> start;

	struct eviction_set *copy = new_eviction_set(es->size);
	copy_eviction_set(copy, es);

	for (size_t i = 1; i < num_xors; i++) {
		for (size_t j = 0; j < es->size; j++) {
			copy->addrs[j] = es->addrs[j] ^ (i << start);
		}

		if (fast_chase(copy, info, stats, PATTERN_FAST_CHASE_MISSES,
			       NULL, NULL)) {
			printp("got %.2f misses for mask 0x%lx\n",
			       PATTERN_FAST_CHASE_MISSES, i << start);
			return copy;
		}
	}

	free_eviction_set(copy);
	return NULL;
}

static struct eviction_set *find_counter_set(struct eviction_set *es,
					     unsigned long first,
					     struct llc_info *info,
					     struct bank_stats *stats)
{
	assert(es);

	unsigned long permus[DRAMA_MAX_NUM_FUNCTIONS] = { 0 };

	struct eviction_set *fs = new_eviction_set(es->size);

	struct eviction_set *ees = new_eviction_set(es->size);
	struct eviction_set *ffs = new_eviction_set(es->size);

	/* Last one is zero */
	size_t num_permus = find_permus_of_other_bank_functions(
		info, stats, permus, DRAMA_MAX_NUM_FUNCTIONS - 1);

	unsigned long mask = find_double_sided_bank_function(stats);

	for (size_t i = 0; i < fs->size; i++) {
		fs->addrs[i] = es->addrs[i] ^ mask;
	}

	/*
	 * permu: change bank/possible a set correction
	 *
	 * es: original
	 * fs: +2 for each row
	 * ees: es + permu
	 * ffs: fs + permu
	 * eees: ees + set mutation
	 * fffs: ffs + set mutation
	 *
	 * We only care about eees and fffs in the end!
	 */

	/* <= so that we include the last one, zero */
	for (size_t i = 0; i <= num_permus; i++) {
		struct eviction_set *eees = NULL;
		struct eviction_set *fffs = NULL;

		for (size_t j = 0; j < es->size; j++) {
			ees->addrs[j] = es->addrs[j] ^ permus[i];
			ffs->addrs[j] = fs->addrs[j] ^ permus[i];
		}

		/*
		 * Changing the set is OK here because it's the counter set! So
		 * it will map to a different cache set than the original,
		 * which was fixed to a set dueling set
		 */
		if (!fast_chase(ees, info, stats, PATTERN_FAST_CHASE_MISSES,
				NULL, NULL)) {
			eees = eviction_set_sweep_set_bits(ees, (1UL << 30) - 1,
							   info, stats);
			if (!eees) continue;
		} else {
			eees = ees;
		}

		if (!fast_chase(ffs, info, stats, PATTERN_FAST_CHASE_MISSES,
				NULL, NULL)) {
			fffs = eviction_set_sweep_set_bits(ffs, (1UL << 30) - 1,
							   info, stats);
			if (!fffs) continue;
		} else {
			fffs = ffs;
		}

		/* Done: update es in place, return fffs */
		for (size_t j = 0; j < es->size; j++) {
			es->addrs[j] = eees->addrs[j];
		}

		fffs->bank = es->bank;

		return fffs;
	}

	/* Sweep row bits without changing the bank... */
	struct eviction_set *gs = ____eviction_set_sweep_row_bits(
		es, (1UL << 30) - 1, info, stats);

	if (gs) {
		/* Duplicate */
		if (gs->addrs[0] == first) {
			free_eviction_set(gs);
			return NULL;
		} else {
			free_eviction_set(es);
			return find_counter_set(gs, first, info, stats);
		}
	} else {
		return NULL;
	}
}

static int pool_is_in(struct eviction_set *pool, unsigned long addr,
		      struct llc_info *info)
{
	for (struct eviction_set *es = pool; es; es = es->next) {
		for (size_t i = 0; i < es->size; i++) {
			if (!((es->addrs[i] ^ addr) >>
			      (info->num_line_bits + info->num_set_bits))) {
				return 1;
			}
		}
	}

	return 0;
}

static unsigned long eviction_set_sweep_row_bits_preserve_bank_set(
	struct eviction_set *es, size_t index,
	unsigned long available_mask_bits, struct llc_info *info,
	struct bank_stats *stats, struct eviction_set *pool)
{
	size_t nonoverlapping_bank_mask = create_bank_mask(info, stats, false);
	size_t start =
		tzb(stats->row_mask & ~(create_bank_mask(info, stats, true) ^
					nonoverlapping_bank_mask));
	size_t stop = tzb(available_mask_bits + 1);
	size_t available_row_mask = ((1UL << (stop - start)) - 1) << start;
	size_t num_xors = (available_row_mask + 1) >> start;

	struct eviction_set *copy = new_eviction_set(es->size);
	copy_eviction_set(copy, es);

	for (size_t i = 1; i < num_xors; i++) {
		unsigned long mask = i << start;
		double misses_if_fail = 0;

		if (mask & nonoverlapping_bank_mask) {
			mask |= nonoverlapping_bank_mask;
		}

		copy->addrs[index] = es->addrs[index] ^ mask;

		if (is_in(es->addrs, es->size, copy->addrs[index])) {
			continue;
		} else if (pool_is_in(pool, copy->addrs[index], info)) {
			continue;
		} else if (fast_chase_swap(copy, index, info, stats,
					   PATTERN_FAST_CHASE_MISSES,
					   &misses_if_fail, NULL)) {
			free_eviction_set(copy);
			return mask;
		}
	}

	free_eviction_set(copy);
	return 0;
}

static struct eviction_set *
eviction_set_expand(struct eviction_set *smaller, size_t new_size,
		    unsigned long available_mask_bits, struct llc_info *info,
		    struct bank_stats *stats)
{
	assert(smaller->size < new_size);

	size_t start = second_lsb_row_mask(stats->row_mask);
	size_t stop = nzb(available_mask_bits);

	struct eviction_set *larger = new_eviction_set(new_size);
	memcpy(larger->addrs, smaller->addrs,
	       smaller->size * sizeof(unsigned long));

	larger->size = smaller->size;

	assert(fast_chase(smaller, info, stats, PATTERN_FAST_CHASE_MISSES, NULL,
			  NULL));

	for (size_t i = 0; i < larger->size; i++) {
		unsigned long old = larger->addrs[i];

		for (size_t mask_width = 1; mask_width < stop; mask_width++) {
			for (size_t j = stop - mask_width - 1; j >= start;
			     j--) {
				for (size_t mask = 0; mask < 1UL << mask_width;
				     mask++) {
					unsigned long new =
						larger->addrs[i] ^ (mask << j);

					assert(larger->size < new_size);
					larger->addrs[larger->size] = new;

					if (is_in(larger->addrs, larger->size,
						  new)) {
						continue;
					} else {
						larger->size++;
					}

					if (fast_chase(larger, info, stats,
						       PATTERN_FAST_CHASE_MISSES,
						       NULL, NULL)) {
						printp("candidate 0x%lx (%lu, %lu, %lu): %.2f\n",
						       new, i, mask_width, j,
						       PATTERN_FAST_CHASE_MISSES);
					} else {
						larger->size--;
						assert(larger->size >=
						       smaller->size);
						continue;
					}

					if (larger->size == new_size) {
						return larger;
					}
				}
			}
		}
	}

	assert(0);
}

static int do_eviction_sets_overlap(struct eviction_set *es,
				    struct eviction_set *fs,
				    struct llc_info *info)
{
	for (size_t i = 0; i < es->size; i++) {
		for (size_t j = 0; j < fs->size; j++) {
			if (!((es->addrs[i] ^ fs->addrs[j]) >>
			      info->num_line_bits)) {
				return 1;
			}
		}
	}

	return 0;
}

static int pool_add_eviction_sets(struct eviction_set **pool,
				  struct eviction_set *xs,
				  struct eviction_set *ys,
				  struct llc_info *info)
{
	assert(pool);

	if (do_eviction_sets_overlap(xs, ys, info)) {
		return 1;
	} else if (!*pool) {
		*pool = xs;
		xs->next = ys;
		return 0;
	}

	for (struct eviction_set *es = *pool; es; es = es->next) {
		unsigned long set = set_is(es->addrs[0], info);

		/*
		 * May be possible to combine them somehow, but let's
		 * not go there
		 */
		if (set_is(xs->addrs[0], info) == set ||
		    set_is(ys->addrs[0], info) == set) {
			return 1;
		} else if (do_eviction_sets_overlap(es, xs, info) ||
			   do_eviction_sets_overlap(es, ys, info)) {
			return 1;
		} else if (!es->next) {
			es->next = xs;
			xs->next = ys;
			return 0;
		}
	}

	assert(0);

	return 1;
}

static size_t pool_total_size(struct eviction_set *pool)
{
	assert(pool);

	size_t size = 0;

	for (struct eviction_set *es = pool; es; es = es->next) {
		size += es->size;
	}

	assert(size % pool->size == 0);

	return size;
}

static void pool_list_all_rows(struct eviction_set *pool, struct llc_info *info,
			       struct bank_stats *stats)
{
	assert(pool);

	size_t size = pool_total_size(pool);

	unsigned long *rows = calloc(size, sizeof(unsigned long));
	unsigned long *banks = calloc(size, sizeof(unsigned long));
	unsigned long *sets = calloc(size, sizeof(unsigned long));

	size_t *indices = calloc(size, sizeof(size_t));

	assert(rows);
	assert(banks);
	assert(sets);
	assert(indices);

	size_t i = 0;

	for (struct eviction_set *es = pool; es; es = es->next) {
		for (size_t j = 0; j < es->size; j++, i++) {
			rows[i] = row_is(es->addrs[j], stats->row_mask);
			banks[i] = bank_is(es->addrs[j], stats->bank_functions);
			sets[i] = set_is(es->addrs[j], info);
		}
	}

	assert(i == size);

	gsl_sort_index(indices, (double *)rows, 1, size);

	for (size_t i = 0; i < size; i++) {
		printf("%05lu,%02lu,%05lu ", rows[indices[i]],
		       banks[indices[i]], sets[indices[i]]);

		for (size_t j = i + 1; j < size; j++) {
			if (banks[indices[j]] == banks[indices[i]]) {
				unsigned long diff =
					rows[indices[j]] - rows[indices[i]];

				if (diff == 2) {
					printf("%02lu <--", diff);
					break;
				} else if (diff > 0) {
					printf("%02lu", diff);
					break;
				}
			}
		}

		printf("\n");
	}
}

/* 
 * Try to change as much addresses to bank as possible
 *
 * If bank == -1, do Sledgehammer
 */
struct eviction_set *eviction_set_change_bank(struct eviction_set *es,
					      ssize_t __bank,
					      struct llc_info *info,
					      struct bank_stats *stats,
					      struct eviction_set *pool)
{
	/*
	 * Could be made more general though, but we rely on this for now.
	 * Update: don't get why this is not general?
	 */
	assert(es);
	assert(es->size >= PATTERN_NUM_LANES_PER_EVICTION_SET * info->wayness);

	struct eviction_set *fs = new_eviction_set(es->size);

	copy_eviction_set(fs, es);

	{
		/* Sanity check? */
		size_t tries = 4;

		while (!fast_chase(fs, info, stats, PATTERN_FAST_CHASE_MISSES,
				   NULL, NULL)) {
			tries--;

			if (!tries) return NULL;
		};
	}

	size_t j = 0;
	size_t num_banks = count_banks(stats);

	for (size_t i = 0; i < es->size; i++) {
		size_t bank;

		if (__bank > 0) {
			bank = __bank;
		} else if (i == 0) {
			unsigned long first = 0;

			assert(!j);

			for (size_t k = 0; k < fs->size; k++) {
				if (!pool_is_in(pool, fs->addrs[k], info)) {
					first = fs->addrs[k];
					fs->addrs[k] = fs->addrs[0];
					fs->addrs[0] = first;
					break;
				}
			}

			if (!first) goto __eviction_set_change_bank_failed;

			j++;
			continue;
		} else {
			bank = (bank_is(fs->addrs[j - 1],
					stats->bank_functions) +
				1) %
			       num_banks;
		}

		printf("\r");
		printp("changing bank (%lu) for address %lu/%lu", bank, i,
		       es->size - 1);

	__eviction_set_change_bank_again:
		if (bank_is(fs->addrs[i], stats->bank_functions) == bank) {
			j++;
			continue;
		}

		unsigned long addr = bank_try_become_keep_set(
			fs->addrs[i], bank, info, stats);

		if (__bank < 0 && !addr) {
			bank = (bank + 1) % num_banks;

			if (bank ==
			    bank_is(fs->addrs[j - 1], stats->bank_functions)) {
				/* We came full circle */
				goto __eviction_set_change_bank_failed;
			} else {
				goto __eviction_set_change_bank_again;
			}
		}

		if (addr && !is_in(fs->addrs, fs->size, addr)) {
			fs->addrs[j] = addr;

			assert(bank_is(addr, stats->bank_functions) == bank);

			/* Just works? */
			if (!fast_chase_swap(fs, j, info, stats,
					     PATTERN_FAST_CHASE_MISSES, NULL,
					     NULL)) {
				/* Works after mask update? */
				unsigned long mask =
					eviction_set_sweep_row_bits_preserve_bank_set(
						fs, j, (1UL << 30) - 1, info,
						stats, pool);

				/* Doesn't work at all, okay... */
				if (!mask) {
					continue;
				} else {
					fs->addrs[j] = addr ^ mask;
				}
			}

			/* In this case it worked: filter */
			if (!pool_is_in(pool, fs->addrs[j], info)) {
				j++;
			}
		}

		/* Won't make it anyway: i has been consumed here, hence -1 */
		if ((j + (fs->size - i - 1)) <
		    PATTERN_NUM_LANES_PER_EVICTION_SET *
			    PATTERN_CHASE_HIGH_DENSITY) {
			break;
		}
	}

	printf("\n");

	assert(j <= es->size);

	/* Did we get enough? */
	if (j >=
	    PATTERN_NUM_LANES_PER_EVICTION_SET * PATTERN_CHASE_HIGH_DENSITY) {
		fs->bank = __bank;
		return fs;
	} else {
	__eviction_set_change_bank_failed:
		free_eviction_set(fs);
		return NULL;
	}
}

/*
 * Should no longer be necessary
 *
 * static bool eviction_sets_are_hammer_pair(struct eviction_set *es,
 *                                           struct eviction_set *fs,
 *                                           struct bank_stats *stats)
 * {
 *         for (size_t i = 0; i < es->size; i++) {
 *                 unsigned long bank =
 *                         bank_is(es->addrs[i], stats->bank_functions);
 *                 unsigned long row = row_is(es->addrs[i], stats->row_mask);
 *
 *                 for (size_t j = 0; j < fs->size; j++) {
 *                         unsigned long other_bank =
 *                                 bank_is(fs->addrs[j], stats->bank_functions);
 *
 *                         if (bank == other_bank) {
 *                                 unsigned long other_row =
 *                                         row_is(fs->addrs[j], stats->row_mask);
 *
 *                                 if (labs(other_row - row) == 2) {
 *                                         printf("Will hammer rows %lu and %lu\n",
 *                                                row, other_row);
 *
 *                                         [> Move both of them to the front <]
 *                                         unsigned long tmp = es->addrs[0];
 *                                         es->addrs[0] = es->addrs[i];
 *                                         es->addrs[i] = tmp;
 *
 *                                         tmp = fs->addrs[0];
 *                                         fs->addrs[0] = fs->addrs[j];
 *                                         fs->addrs[j] = tmp;
 *
 *                                         return true;
 *                                 }
 *                         }
 *                 }
 *         }
 *
 *         return false;
 * }
 */

enum data_pattern_label { COLSTRIPE, CHECKERED, ROWSTRIPE, NUM_DATA_PATTERNS };

static size_t data_pattern_max(void)
{
	return NUM_DATA_PATTERNS * 2;
}

static void data_pattern_iter(struct data_pattern *dp, size_t index)
{
	size_t i = 0;

	assert(index < data_pattern_max());

	for (size_t a = 0; a < NUM_DATA_PATTERNS; a++) {
		for (size_t b = 0; b < 2; b++) {
			if (index == i) {
				if (a == COLSTRIPE) {
					dp->a = 0x55;
					dp->v = 0x55;
				} else if (a == CHECKERED) {
					dp->a = 0x55;
					dp->v = 0xaa;
				} else if (a == ROWSTRIPE) {
					dp->a = 0x00;
					dp->v = 0xff;
				}

				if (b) {
					dp->a ^= 0xff;
					dp->v ^= 0xff;
				}

				dp->index = index;

				return;
			} else {
				i++;
			}
		}
	}
}

/*
 * Not used anymore, was used to pause for a REF in the multicore case.
 * Multicore case does everything at the same time now. The single core case
 * uses ordinary NOPs and doesn't wait whole REFs
 */
static void nop_sync(void)
{
	size_t num_nops = 30000;

	struct timespec start = { 0 };
	struct timespec stop = { 0 };

	unsigned long **xs = calloc(2, sizeof(unsigned long *));

	xs[0] = (unsigned long *)&xs[1];
	xs[1] = (unsigned long *)&xs[0];

	while (1) {
		double diff;

		clock_gettime(CLOCK_MONOTONIC_RAW, &start);

		for (size_t i = 0; i < PATTERN_NUM_LOOPS_SYNC; i++) {
			for (size_t j = 0; j < num_nops; j++) {
				asm volatile(
					"mov (%[x]), %[x]" ::[x] "r"(xs[0]));
			}
		}

		clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

		double nanos_per_loop =
			(double)((1000000000 * (stop.tv_sec - start.tv_sec)) +
				 (stop.tv_nsec - start.tv_nsec)) /
			PATTERN_NUM_LOOPS_SYNC;

		if (nanos_per_loop < 7800) {
			printf("\r%5.0f ns: bumping", nanos_per_loop);
			/* Bump */
			num_nops += 5000;
			continue;
		} else {
			diff = nanos_per_loop - 7800;
		}

		/* Without the prints below we get about 18K NOPs...? */
		if (diff <= 10) {
			printf("\r%5lu NOPs @ %5.0f ns\n", num_nops,
			       nanos_per_loop);
			/*
			 * The times 2 is magic, probably has to do with what
			 * the compiler makes of our actual loop in __hammer.
			 *
			 * We can also get it if we enable the printps below
			 */
			cpu_nops_per_ref = num_nops;
			free(xs);
			return;
		} else if (diff > 1000) {
			num_nops -= 1000;
		} else if (diff > 500) {
			num_nops -= 500;
		} else {
			num_nops -= 50;
		}
	}
}

static ssize_t __refblock_sync(struct refblock *blk)
{
	unsigned long *xs = blk->cs[0].instance.addrs;
	unsigned long *ys = blk->cs[1].instance.addrs;

	assert(xs);
	assert(ys);

	/* Cannot be zero */
	ssize_t nops_per = 1;
	ssize_t successes = PATTERN_SYNC_SUCCESSES;

	for (size_t t = 0; t < PATTERN_NUM_TRIES_SYNC; t++) {
		double micros[PATTERN_NUM_LOOPS_SYNC] = { 0 };

		for (size_t i = 0; i < PATTERN_NUM_LOOPS_SYNC; i++) {
			struct timespec start = { 0 };
			struct timespec stop = { 0 };

			clock_gettime(CLOCK_MONOTONIC_RAW, &start);

			if (blk->cs[0].rainbow) {
#ifdef RAINBOW2
				asm_hammer_evict_loop_single_block_rainbow2(
					xs[0], xs[1], ys[0], ys[1],
					blk->reps_per_ref, nops_per);
#else
				asm_hammer_evict_loop_single_block_rainbow(
					xs[0], xs[1], xs[2], xs[3], ys[0],
					ys[1], ys[2], ys[3], blk->reps_per_ref,
					nops_per);
#endif
			} else {
				asm_hammer_evict_loop_single_block(
					xs[0], ys[0], blk->reps_per_ref,
					nops_per, blk->__press_nops_per,
					blk->cs[0].interleave);
			}

			clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

			micros[i] = (double)((1000000000 *
					      (stop.tv_sec - start.tv_sec)) +
					     (stop.tv_nsec - start.tv_nsec)) /
				    1000;
		}

		double median =
			gsl_stats_median(micros, 1, PATTERN_NUM_LOOPS_SYNC);
		double q = median / 7.8;
		double diff = fabs(PATTERN_SYNC_MAX_REFS - q);

		if (q >= sync_max_refs(PATTERN_SYNC_MAX_REFS)) {
			size_t reduced = reps_per_ref_reduce(
				blk->reps_per_ref, blk->__non_uniformity);

			if (reduced >=
			    reps_per_ref_min(blk->__non_uniformity)) {
				blk->reps_per_ref = reduced;
				continue;
			} else {
				return -1;
			}
		} else {
			if (diff < PATTERN_SYNC_ACCURACY) {
				successes--;

				if (successes == 0) {
					assert(nops_per >= 1);
					assert(nops_per <
					       (PATTERN_SYNC_MAX_NOPS /
						SPLIT_DETECT_CONST));
					return nops_per;
				}
			} else {
				if (q > PATTERN_SYNC_MAX_REFS) {
					nops_per -=
						diff / PATTERN_SYNC_ACCURACY;
				} else {
					nops_per += 2 * (diff /
							 PATTERN_SYNC_ACCURACY);
				}
			}
		}

		if (nops_per >= PATTERN_SYNC_MAX_NOPS / SPLIT_DETECT_CONST) {
			break;
		}

		if (nops_per < 1) {
			break;
		}
	}

#ifdef __TARGET_SEED
	return __refblock_sync(blk);
#else
	printf("%s: out of tries\n", __func__);
	return -1;
#endif
}

static void pattern_install(struct pattern *p)
{
	for (size_t i = 0; i < p->num_blks + 1; i++) {
		struct refblock *blk;

		if (i < p->num_blks) {
			blk = &p->blks[i];
		} else if (p->trigger.enabled) {
			blk = &p->trigger.blk;
		} else {
			break;
		}

		if (blk->self == blk) refblock_install(blk);
	}
}

static void pattern_reduce_reps(struct pattern *p)
{
	for (size_t i = 0; i < p->num_blks; i++) {
		struct refblock *blk = &p->blks[i];

		if (blk->self != blk) continue;

		assert(blk->reps_per_ref);
		assert(blk->reps_per_ref % (blk->__non_uniformity + 1) == 0);

		size_t reduced = reps_per_ref_reduce(blk->reps_per_ref,
						     blk->__non_uniformity);

		if (reduced >= reps_per_ref_min(blk->__non_uniformity)) {
			blk->reps_per_ref = reduced;
		} else {
			continue;
		}

		assert(blk->reps_per_ref);
		assert(blk->reps_per_ref % (blk->__non_uniformity + 1) == 0);
	}
}

/* TODO: broken? */
static size_t pattern_count_victim_rows(struct pattern *p)
{
	size_t num_victim_rows = 0;

	for (size_t i = 0; i < p->num_blks + 1; i++) {
		struct refblock *blk = &p->blks[i];

		if (i < p->num_blks) {
			blk = &p->blks[i];
		} else if (p->trigger.enabled) {
			blk = &p->trigger.blk;
		} else {
			break;
		}

		if (blk->self != blk) continue;

		for (size_t j = 0; j < PATTERN_NUM_CHASES; j++) {
			/*
			 * Overapproximation: the middle victim row counts
			 * twice
			 */
			num_victim_rows += blk->cs[j].misses_per_set * 2;
		}
	}

	return num_victim_rows;
}

static int pattern_sync(struct pattern *p, ssize_t r)
{
	/* This should avoid a resync while sweeping */
	if (p->multiblock.num_refs) return;

	ssize_t nops_per = 1;
	ssize_t successes = PATTERN_SYNC_SUCCESSES;

	if (r < 0) return 1;

	assert(nops_per);

	for (size_t t = 0; t < PATTERN_NUM_TRIES_SYNC; t++) {
		double micros[PATTERN_NUM_LOOPS_SYNC] = { 0 };

		p->multiblock.nops_per = nops_per;

		for (size_t i = 0; i < PATTERN_NUM_LOOPS_SYNC; i++) {
			struct timespec start = { 0 };
			struct timespec stop = { 0 };

			clock_gettime(CLOCK_MONOTONIC_RAW, &start);

			hammer_times(p, 1);

			clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

			micros[i] = (double)((1000000000 *
					      (stop.tv_sec - start.tv_sec)) +
					     (stop.tv_nsec - start.tv_nsec)) /
				    1000;
		}

		double median =
			gsl_stats_median(micros, 1, PATTERN_NUM_LOOPS_SYNC);
		double q = median / 7.8;
		double diff = fabs(p->multiblock.num_refs - q);

		if (q >= sync_max_refs(PATTERN_SYNC_MAX_REFS_MULTIBLOCK)) {
		pattern_sync_halve:
			p->multiblock.num_refs = 0;
			pattern_reduce_reps(p);
			return pattern_sync(p, r - 1);
		} else if (p->multiblock.num_refs == 0) {
			if (ceil(q) >= PATTERN_SYNC_MAX_REFS_MULTIBLOCK) {
				goto pattern_sync_halve;
			}

			double target =
				ceil(q) +
				(double)(rand() %
					 (PATTERN_SYNC_MAX_REFS_MULTIBLOCK -
					  (int)ceil(q)));

			/* We don't do more than 1! */
			p->multiblock.extra = target - q;

			while (p->multiblock.extra > 1) {
				p->multiblock.extra--;
			}

			p->multiblock.num_refs = ceil(q + p->multiblock.extra);

			assert(p->multiblock.num_refs >= 1);
			assert(p->multiblock.num_refs <
			       PATTERN_SYNC_MAX_REFS_MULTIBLOCK);
		} else {
			if (diff < PATTERN_SYNC_ACCURACY) {
				successes--;

				if (successes == 0) {
					assert(nops_per >= 1);
					assert(nops_per <
					       (PATTERN_SYNC_MAX_NOPS /
						SPLIT_DETECT_CONST));
					return 0;
				}
			} else {
				if (q > p->multiblock.num_refs) {
					nops_per -=
						diff / PATTERN_SYNC_ACCURACY;
				} else {
					nops_per += 8 * (diff /
							 PATTERN_SYNC_ACCURACY);
				}
			}
		}

		if (nops_per >= PATTERN_SYNC_MAX_NOPS / SPLIT_DETECT_CONST) {
			break;
		}

		if (nops_per < 1) {
			break;
		}
	}

#ifdef __TARGET_SEED
	return pattern_sync(p, r);
#else
	printf("%s: out of tries\n", __func__);
	return 1;
#endif
}

static int refblock_sync(struct refblock *blk)
{
	ssize_t nops_per = __refblock_sync(blk);

	/* 0: means not stable, -1: does not fit, or out of tries */
	if (nops_per <= 0) {
		return 1;
	} else {
		blk->nops_per_per_ref = nops_per;
		return 0;
	}
}

/*
 * static void pattern_hc_init(struct pattern *p)
 * {
 *         p->num_tries_hammer = PATTERN_NUM_TRIES_HAMMER_MIN_POW;
 *         p->num_trefi_factor = 4;
 *         p->base_delay_trefi = 4;
 * }
 */

/*
 * static void pattern_hc_rand(struct pattern *p)
 * {
 *         p->num_tries_hammer = rand() % (PATTERN_NUM_TRIES_HAMMER_MAX_POW -
 *                                         PATTERN_NUM_TRIES_HAMMER_MIN_POW) +
 *                               PATTERN_NUM_TRIES_HAMMER_MIN_POW;
 *         p->num_trefi_factor =
 *                 rand() % (PATTERN_NUM_TREFI_HAMMER_FACTOR_MAX_POW -
 *                           PATTERN_NUM_TREFI_HAMMER_FACTOR_MIN_POW) +
 *                 PATTERN_NUM_TREFI_HAMMER_FACTOR_MIN_POW;
 * 
 * pattern_hc_rand_again:
 *         p->base_delay_trefi = rand() % (PATTERN_BASE_DELAY_TREFI_MAX_POW -
 *                                         PATTERN_BASE_DELAY_TREFI_MIN_POW) +
 *                               PATTERN_BASE_DELAY_TREFI_MIN_POW;
 * 
 *         while ((p->base_delay_trefi - PATTERN_BASE_DELAY_TREFI_MIN_POW) %
 *                PATTERN_BASE_DELAY_WINDOW_POW) {
 *                 goto pattern_hc_rand_again;
 *         }
 * }
 */

/* Returns 1 if there's more to do, 0 otherwise */
/*
 * static int pattern_hc_next(struct pattern *p, size_t index)
 * {
 *         size_t k = 0;
 * 
 *         for (size_t i = PATTERN_NUM_TRIES_HAMMER_MIN_POW;
 *              i < PATTERN_NUM_TRIES_HAMMER_MAX_POW; i++) {
 *                 for (size_t j = PATTERN_NUM_TREFI_HAMMER_FACTOR_MIN_POW;
 *                      j < PATTERN_NUM_TREFI_HAMMER_FACTOR_MAX_POW; j++) {
 *                         for (size_t l = PATTERN_BASE_DELAY_TREFI_MIN_POW;
 *                              l < PATTERN_BASE_DELAY_TREFI_MAX_POW;
 *                              l += PATTERN_BASE_DELAY_WINDOW_POW) {
 *                                 if (k == index) {
 *                                         p->num_tries_hammer = i;
 *                                         p->num_trefi_factor = j;
 *                                         p->base_delay_trefi = l;
 *                                         return 0;
 *                                 }
 * 
 *                                 k++;
 *                         }
 *                 }
 *         }
 * 
 *         return 1;
 * }
 */

static int pattern_hc_next_const_time(struct pattern *p, size_t index)
{
	size_t k = 0;

	for (size_t i = PATTERN_NUM_TRIES_HAMMER_MIN_POW;
	     i < PATTERN_NUM_TRIES_HAMMER_MAX_POW; i += 2) {
		for (size_t l = PATTERN_BASE_DELAY_TREFI_MIN_POW;
		     l < PATTERN_BASE_DELAY_TREFI_MAX_POW;
		     l += PATTERN_BASE_DELAY_WINDOW_POW) {
			if (k == index) {
				p->num_tries_hammer = i;
				/*
				 * 2 -> 512
				 * 8 -> 128
				 * 32 -> 32
				 */
				p->num_trefi_factor = 10 - i;
				p->base_delay_trefi = l;
				return 0;
			}

			k++;
		}
	}

	return 1;
}

static size_t pattern_hc_rand(struct pattern *p)
{
	static size_t max = 0;

	if (!max) {
		while (!pattern_hc_next_const_time(p, max)) {
			max++;
		}
	}

	assert(max);

	size_t index = rand() % max;

	pattern_hc_next_const_time(p, index);

	return index;
}

static size_t __reps_per_ref(size_t max, size_t nu)
{
	return rand() % (max - reps_per_ref_min(nu) + 1) + reps_per_ref_min(nu);
}

#ifdef SLEDGEHAMMER
static int pattern_iter_postpone(struct pattern *p)
{
/* From the paper, Fig. 18 */
#define SLEDGEHAMMER_MAX_SIDES 21
#define SLEDGEHAMMER_MIN_SIDES 8
#define SLEDGEHAMMER_MAX_BANKS 9
#define SLEDGEHAMMER_MIN_BANKS 3
	p->seed = rand();
	srand(p->seed);
	p->num_blks = 1;
	p->sets = NULL;
	p->num_sets = 0;

	p->blks[0].id = 0;
	p->blks[0].self = &p->blks[0];

	p->sledgehammer.enabled = true;
	p->sledgehammer.banks =
		(rand() % (SLEDGEHAMMER_MAX_BANKS - SLEDGEHAMMER_MIN_BANKS)) +
		SLEDGEHAMMER_MIN_BANKS;
	p->sledgehammer.sides =
		(rand() % (SLEDGEHAMMER_MAX_SIDES - SLEDGEHAMMER_MIN_SIDES)) +
		SLEDGEHAMMER_MIN_SIDES;
	p->sledgehammer.set = rand() % PATTERN_NUM_EVICTION_SETS;

	return 1;
}
#else
#ifdef __TARGET_SEED
static size_t target = 0x7aa9d8df;
#endif
static int pattern_iter_postpone(struct pattern *p)
{
#ifdef __TARGET_SEED
	size_t seed;

	if (target) {
		seed = target;
		target = 0;
	} else {
		seed = rand();
	}
#else
	size_t seed = rand();
#endif

	p->seed = seed;
	assert(!(0xffffffff00000000UL & p->seed));
	srand(seed);

	p->hc_index = pattern_hc_rand(p);

#ifdef __MICRON_PREMULTI
	size_t tmp = 0;
#else
	size_t tmp = rand() % 20;
#endif

	if (tmp < 12) {
		p->num_blks = 1;
	} else if (tmp < 19) {
		p->num_blks = 2;
	} else {
		assert(PATTERN_NUM_BLKS_MIN > 2);
		p->num_blks = (rand() % (PATTERN_NUM_BLKS_MAX -
					 PATTERN_NUM_BLKS_MIN + 1)) +
			      PATTERN_NUM_BLKS_MIN;
	}

#ifdef MULTIBLOCK_ONLY
	p->num_blks = rand() % 2 + 1;
	p->unique = 1;

	p->interleave = 0;
	p->rainbow = 0;
	p->gather = rand() % 2;
	p->multiblock.enabled = rand() % 2;
	p->trigger.enabled = 0;
#elif defined(SPLIT_DETECT)
	p->num_blks = 1;
	p->unique = 1; /* Doesn't really matter */

	p->interleave = 1;
	p->rainbow = 0;
	p->gather = 0;
	p->multiblock.enabled = 1;
	p->trigger.enabled = 0;
#else
	p->unique = rand() % 2;

	/* Let's not do this too often */
	p->interleave = !!(rand() % 4);

	/* If we interleave, we might do rainbow */
	p->rainbow = p->interleave ? !!!(rand() % 8) : 0;
	p->gather = !!!(rand() % 10);

	/* If we interleave, we might do multiblock */
	p->multiblock.enabled = p->interleave ? rand() % 2 : 0;
	p->trigger.enabled = p->multiblock.enabled ? rand() % 2 : 0;

	/* If we interleave, we don't do Rowpress! */
	if (p->interleave || !!(rand() % 3)) {
		p->press_nops_per = 0;
	} else {
		p->press_nops_per = rand() % 4 + 1;
	}

	assert(!(p->interleave && p->press_nops_per));
#endif

	if (p->sets) free(p->sets);
	p->sets = NULL;
	p->num_sets = 0;

	/* Which? */
	for (size_t i = 0; i < p->num_blks; i++) {
		p->blks[i].id = p->unique ? i : rand() % p->num_blks;
		p->blks[i].self = NULL;
	}

	for (size_t i = 0; i < p->num_blks; i++) {
		for (size_t j = 0; j < i; j++) {
			if (p->blks[i].id == p->blks[j].id) {
				p->blks[i].self = &p->blks[j];
				break;
			}
		}

		if (!p->blks[i].self) {
			p->blks[i].self = &p->blks[i];
		}

		assert(p->blks[i].self);
	}

	/* Misses per set? */
	for (size_t i = 0; i < p->num_blks; i++) {
		struct refblock *blk = &p->blks[i];

		if (blk->self != blk) continue;

		/*
		 * We had this, at most one mixed chase:
		 *
		 * if (has_mixed_chase) {
		 *         blk->ct = EQUAL;
		 * } else {
		 *         blk->ct = rand() % NUM_CHASE_TYPES;
		 *         has_mixed_chase = blk->ct != EQUAL;
		 * }
		 *
		 * But they seemed less effective and are hard to plot.
		 * Let's do normal double chases only
		 */
		blk->ct = EQUAL;

		/*
		 * 0: A, B, A, B (uniform: 50/50)
		 * 1: A, B, A, C (non-uniform: 50/25/25)
		 * 2: A, B, A, B, A, C (non-uniform: 50/33/16)
		 * 3: A, B, A, B, A, B, A, C (non-uniform: 50/38/12)
		 */
#if defined(MULTIBLOCK_ONLY) || defined(SPLIT_DETECT)
		blk->__non_uniformity = 0;
#else
		blk->__non_uniformity = rand() % 4;
#endif
		blk->__press_nops_per = p->press_nops_per;

		for (size_t i = 0; i < PATTERN_NUM_CHASES; i++) {
			struct chase *c = &blk->cs[i];
			free_chase_instance(c);

			c->hit_reps = 1;
			c->non_uniformity = blk->__non_uniformity;
			c->size = non_uniformity_to_lanes(c->non_uniformity) *
				  c->hit_reps * __WAYNESS;
			c->gather = p->gather;
			c->interleave = p->interleave;
			c->rainbow = p->rainbow;

			if (c->flips_around_index) {
				free(c->flips_around_index);
			}

			c->flips_around_index = calloc(c->size, sizeof(size_t));
			assert(c->flips_around_index);

			if (c->miss_at_index) {
				free(c->miss_at_index);
			}

			c->miss_at_index = calloc(c->size, sizeof(bool));
			assert(c->miss_at_index);

			if (i == 0) {
#ifdef SPLIT_DETECT
				c->misses_per_set =
					SPLIT_DETECT_NUM_MISSES_PER_SET_BLK;
#else
				if (c->rainbow) {
					/*
					 * Because we do four in
					 * parallel!
					 */
#ifdef RAINBOW2
					assert(__MISSES_PER_SET_MAX_RAINBOW ==
					       12);
					c->misses_per_set =
						((rand() % 6) + 1) * 2;
#else
					assert(__MISSES_PER_SET_MAX_RAINBOW ==
					       12);
					c->misses_per_set =
						((rand() % 3) + 1) * 4;
#endif
				} else if (blk->__press_nops_per) {
					c->misses_per_set =
						(rand() %
						 (2 - MISSES_PER_SET_MIN + 1)) +
						MISSES_PER_SET_MIN;
				} else {
					c->misses_per_set =
						(rand() %
						 (MISSES_PER_SET_MAX -
						  MISSES_PER_SET_MIN + 1)) +
						MISSES_PER_SET_MIN;
				}
#endif
			} else {
				assert(blk->ct == EQUAL);
				c->misses_per_set = blk->cs[0].misses_per_set;
			}
		}

		{
			/* Low, random, high */
			size_t x = rand() % 3;

#ifdef SPLIT_DETECT
			x = 1;
#endif

			if (x == 0) {
				blk->reps_per_ref =
					__reps_per_ref(__REPS_PER_REF_LOW_MAX,
						       blk->__non_uniformity);
			} else if (x == 1) {
				blk->reps_per_ref =
					__reps_per_ref(REPS_PER_REF_MAX,
						       blk->__non_uniformity);
			} else {
				blk->reps_per_ref = REPS_PER_REF_MAX;
			}
		}

		while (blk->reps_per_ref % (blk->__non_uniformity + 1)) {
			blk->reps_per_ref++;
		}

		assert(blk->reps_per_ref);
		assert(blk->reps_per_ref % (blk->__non_uniformity + 1) == 0);

		if (p->multiblock.enabled || p->num_blks == 1) {
			blk->reps_of_blk = 1;
		} else {
			blk->reps_of_blk = (rand() % (REPS_OF_BLK_MAX -
						      REPS_OF_BLK_MIN + 1)) +
					   REPS_OF_BLK_MIN;
		}
	}

	return 1;
}
#endif /* SLEDGEHAMMER */

static ssize_t __row_chk_set(unsigned long addr, size_t bank, size_t row,
			     unsigned char val, bool chk, ssize_t dist,
			     struct bank_stats *stats)
{
	unsigned long column_mask = create_column_mask(stats);
	ssize_t num_flips = 0;

	addr = row_becomes(addr, row, stats->row_mask);
	addr = bank_become_keep_row(addr, bank, stats);

	assert(addr);

	size_t i = 0;

	for (size_t c = 0; c < 1UL << nzb(column_mask); c++) {
		addr = column_becomes_keep_bank(addr, c, column_mask, stats);
		addr = addr ^ (addr & 0x7UL);
		assert(addr % 8 == 0);

		for (size_t bo = 0; bo < 8; bo++) {
			addr = (addr ^ (addr & 0x7UL)) ^ bo;
			assert(addr % 8 == bo);

			assert(column_is(addr, column_mask) == c);
			assert(bank_is(addr, stats->bank_functions) == bank);
			assert(row_is(addr, stats->row_mask) == row);

			if (chk) {
				unsigned char got = *(unsigned char *)addr;
				unsigned char diff = got ^ val;

				if (diff) {
					printp("%04lu,%04lu/+%2ld[%04lu]: 0x%02x -> 0x%02x (%1d)\t\t\t\t(*)\n",
					       bank, row, dist, i, val, got,
					       nzb(diff));

					/* Restore! */
					*(unsigned char *)addr =
						(unsigned char)val;
					num_flips++;
				}

				/* Probably a bug if the whole byte flipped */
				assert(nzb(diff) < 8);
			} else {
				*(unsigned char *)addr = val;
			}

			i++;
		}

		if (!chk) {
			if (addr % 64 == 0) {
				asm volatile("clflushopt (%0)\n\t"
					     :
					     : "r"(addr)
					     : "memory");
			}
		}
	}

	/* For all the flushes */
	if (!chk) asm volatile("sfence\n\t");

	return num_flips;
}

/* Ignore this row? */
static bool row_ign(size_t bank, size_t row, struct pattern *p,
		    struct bank_stats *stats)
{
	for (size_t i = 0; i < p->num_blks + 1; i++) {
		struct refblock *blk;

		if (i < p->num_blks) {
			blk = &p->blks[i];
		} else if (p->trigger.enabled) {
			/* Also if no misses, used for pointer chase! */
			blk = &p->trigger.blk;
		} else {
			break;
		}

		if (blk->self != blk) continue;

		for (size_t j = 0; j < PATTERN_NUM_CHASES; j++) {
			if (chase_has_row(&blk->cs[j], bank, row, stats)) {
				return true;
			}
		}
	}

	return false;
}

/* 
 * Returns the number of bit flips or -1 if the row is ignored
 */
static ssize_t row_chk_set(unsigned long addr, size_t bank, ssize_t row,
			   char val, bool chk, ssize_t dist, struct pattern *p,
			   struct bank_stats *stats)
{
	ssize_t num_rows = 1UL << nzb(stats->row_mask);

	if (row < 0 || row >= num_rows) {
		return -1;
	} else if (chk && row_ign(bank, row, p, stats)) {
		/* Only if checking, otherwise, set them! And install later */
		return -1;
	} else {
		return __row_chk_set(addr, bank, row, val, chk, dist, stats);
	}
}

/* See BLASTER */
#define PATTERN_RADIUS 8
static void __update_flip_count(struct pattern *p, size_t row_with_flips,
				size_t num_flips, struct bank_stats *stats)
{
	for (size_t i = 0; i < p->num_blks; i++) {
		struct refblock *blk = &p->blks[i];

		if (blk->self != blk) continue;

		for (size_t j = 0; j < PATTERN_NUM_CHASES; j++) {
			bool *miss_at_index = blk->cs[j].miss_at_index;
			size_t *flips_around_index =
				blk->cs[j].flips_around_index;

			for (size_t k = 0; k < blk->cs[j].size; k++) {
				if (!miss_at_index[k]) continue;

				ssize_t row =
					row_is(blk->cs[j].instance.addrs[k],
					       stats->row_mask);

				for (size_t l = 1; l <= PATTERN_RADIUS; l++) {
					for (ssize_t m = -1; m <= 1; m++) {
						ssize_t r = row + m * l;
						size_t score =
							PATTERN_RADIUS - l + 1;

						if (r == row_with_flips) {
							printf("%lu/%lu/%lu/%lu: adding %lu\n",
							       k, row, r,
							       row_with_flips,
							       num_flips *
								       score);
							flips_around_index[k] +=
								(num_flips *
								 score);
						}
					}
				}
			}
		}
	}
}

static size_t pattern_chk_set_rows(struct pattern *p, bool chk,
				   size_t *__num_checked,
				   struct bank_stats *stats)
{
	size_t total_num_flips = 0;

	/*
	 * 2 * MISSES_PER_SET_MAX because each chase has max. 3 * 16 (A, B, C)
	 * * 2 because each row has two victim rows
	 */
	size_t max_checked = (p->num_blks + 1) * PATTERN_NUM_CHASES *
			     (3 * MISSES_PER_SET_MAX) * (2 * PATTERN_RADIUS);
	struct br *checked = calloc(max_checked, sizeof(struct br));
	size_t num_checked = 0;

	assert(checked);

	/* We don't check the trigger block */
	for (size_t b = 0; b < p->num_blks; b++) {
		struct refblock *blk = &p->blks[b];

		if (blk->self != blk) continue;

		size_t *num_flips = &blk->num_flips;

		for (size_t i = 0; i < PATTERN_NUM_CHASES; i++) {
			unsigned long *addrs = blk->cs[i].instance.addrs;
			bool *miss_at_index = blk->cs[i].miss_at_index;

			for (size_t j = 0; j < blk->cs[i].size; j++) {
				if (!miss_at_index[j]) continue;

				unsigned long addr = addrs[j];
				ssize_t row = row_is(addr, stats->row_mask);
				size_t bank =
					bank_is(addr, stats->bank_functions);

				for (size_t l = 1; l <= PATTERN_RADIUS; l++) {
					for (ssize_t k = -1; k <= 1; k++) {
						ssize_t r = row + k * l;
						char val;

						if (r % 2) {
							val = blk->dp.a;
						} else {
							val = blk->dp.v;
						}

						struct br br = { bank, r };

						assert(num_checked <
						       max_checked);

						if (is_in_br(checked,
							     num_checked, br)) {
							continue;
						}

						ssize_t new = row_chk_set(
							addr, bank, r, val, chk,
							k * l, p, stats);

						/* Got ignored */
						if (new < 0) continue;

						checked[num_checked++] = br;

						if (new > 0) {
							__update_flip_count(
								p, r, new,
								stats);

							*num_flips += new;
							total_num_flips += new;
						}
					}
				}
			}
		}
	}

	free(checked);

	if (__num_checked) *__num_checked = num_checked;

	return total_num_flips;
}

static int double_chase_instance_from_eviction_sets(struct refblock *blk,
						    struct eviction_set *es,
						    struct eviction_set *fs,
						    struct llc_info *info,
						    struct bank_stats *stats)
{
	assert(PATTERN_NUM_CHASES == 2);

	chase_instance_from_eviction_set(es, &blk->cs[0], info, stats);
	chase_instance_from_eviction_set(fs, &blk->cs[1], info, stats);

	return 0;
}

static int pattern_instantiate_chases(struct pattern *p,
				      struct eviction_set **pool,
				      size_t pool_size, struct llc_info *info,
				      struct bank_stats *stats)
{
#define PATTERN_INSTANTIATE_MAX_NUM_TRIES (PATTERN_NUM_EVICTION_SETS / 2)
#define reinstantiate (num_sets_was != 0)
	ssize_t bank = -1;
	size_t num_sets_was = p->num_sets;

	if (reinstantiate) {
		assert(p->sets);
		p->num_sets += p->num_blks;
		p->sets = realloc(p->sets, p->num_sets * sizeof(size_t));
	} else {
		assert(!p->sets);
		/* + 1 because we do the trigger block the first time */
		p->num_sets = p->num_blks + 1;
		p->sets = calloc(p->num_sets, sizeof(size_t));
	}

	assert(p->sets);
	assert(pool_size % PATTERN_NUM_EVICTION_SETS_PER_BLK == 0);

	/* Get one extra: trigger block */
	for (size_t i = 0; i < p->num_blks + 1; i++) {
		struct refblock *blk;

		size_t j = num_sets_was + i;

		if (i < p->num_blks) {
			blk = &p->blks[i];
		} else if (reinstantiate) {
			continue;
		} else {
			assert(i == p->num_blks);

			/* Initialize the trigger block */
			blk = &p->trigger.blk;
			blk->self = blk;
			blk->id = p->num_blks;

			blk->ct = EQUAL;

			for (size_t k = 0; k < PATTERN_NUM_CHASES; k++) {
				struct chase *c = &blk->cs[k];

				free_chase_instance(c);

				c->hit_reps = 1;
				c->size = 2 * c->hit_reps * __WAYNESS;
				c->gather = p->gather;
				c->interleave = p->interleave;

				/* Not used */
				if (c->flips_around_index) {
					free(c->flips_around_index);
					c->flips_around_index = NULL;
				}

				if (c->miss_at_index) {
					free(c->miss_at_index);
				}

				c->miss_at_index =
					calloc(c->size, sizeof(bool));
				assert(c->miss_at_index);

				if (i == 0) {
					c->misses_per_set =
						rand() %
						(SPLIT_DETECT_NUM_MISSES_PER_SET_TRIGGER_MAX +
						 1);
				} else {
					c->misses_per_set =
						blk->cs[0].misses_per_set;
				}
			}

			/* Not applicable */
			blk->reps_per_ref = 0;
			blk->reps_of_blk = 0;
		}

		if (blk->self != blk) {
			assert(i);
			continue;
		} else {
			/* Propagate down, for double_chase_instance_from_eviction_sets */
			for (size_t j = 0; j < PATTERN_NUM_CHASES; j++) {
				struct chase *c = &blk->cs[j];
				free_chase_instance(c);
			}
		}

		size_t tries = 0;

	use_another_set:;
		size_t s = PATTERN_NUM_EVICTION_SETS_PER_BLK *
			   (rand() %
			    (pool_size / PATTERN_NUM_EVICTION_SETS_PER_BLK));

		if (is_in(p->sets, j, s) &&
		    ++tries < PATTERN_INSTANTIATE_MAX_NUM_TRIES) {
			goto use_another_set;
		} else {
			p->sets[j] = s;
			assert(p->sets[j] % 2 == 0);
		}

		/*
		 * Make the bank is the same. Not the first time. Give up if
		 * we've tried too many times
		 *
		 * Rainbow pool? Make sure the first address has the same bank
		 * (we cannot guarantee this)
		 */
		if (i > 0) {
			ssize_t b = pool[p->sets[j]]->bank;

			if (b < 0) {
				b = bank_is(pool[p->sets[j]]->addrs[0],
					    stats->bank_functions);

				/*
				 * Rainbow case
				 *
				 * The pair set might not target the same bank.
				 * That is:
				 *
				 * b != bank_is(pool[p->sets[j] +
				 * 1]->addrs[0], stats->bank_functions));
				 *
				 * Whatever!
				 */
			} else {
				assert(b == pool[p->sets[j] + 1]->bank);
			}

			assert(bank >= 0);

			/* Not the trigger? Same bank! */
			if (blk != &p->trigger.blk && b != bank &&
			    ++tries < PATTERN_INSTANTIATE_MAX_NUM_TRIES) {
				goto use_another_set;
			}
		}

		if (double_chase_instance_from_eviction_sets(
			    blk, pool[p->sets[j]], pool[p->sets[j] + 1], info,
			    stats)) {
			return 1;
		}

		if (i == 0) {
			assert(bank < 0);
			bank = pool[p->sets[j]]->bank;

			if (bank < 0) {
				bank = bank_is(pool[p->sets[j]]->addrs[0],
					       stats->bank_functions);
				/* Pair set's bank might not be the same, see
				 * above, but whatever */
			} else {
				assert(bank == pool[p->sets[j] + 1]->bank);
			}
		}
	}

	return 0;
};

#define __fast(pattern) ((pattern)->blks[F].c.instance)
#define __slow(pattern) ((pattern)->blks[S].c.instance)

struct refblock_t {
	struct refblock *blk;

	unsigned long *masks;
	size_t num_masks;
	size_t mod; /* Until when in the last mask */

	pthread_barrier_t *barrier;

	double us;
};

/* #define NUM_CPUS 6 */
/* #define NUM_THREADS_PER_CPU 2 */

/*
 * #define NUM_CPUS 12
 * #define NUM_THREADS_PER_CPU 1
 *
 * static size_t affin_iter_max(void)
 * {
 *         return NUM_CPUS * (NUM_CPUS - 1);
 * }
 */

/*
 * static void affin_iter(cpu_set_t *sa, cpu_set_t *sb, size_t index)
 * {
 *         size_t i = 0;
 *
 *         for (size_t a = 0; a < NUM_CPUS; a++) {
 *                 for (size_t __b = 1; __b < NUM_CPUS; __b++) {
 *                         size_t b = (a + __b) % NUM_CPUS;
 *                         assert(a != b);
 *
 *                         if (i == index) {
 *                                 CPU_ZERO(sa);
 *                                 CPU_SET(a * NUM_THREADS_PER_CPU, sa);
 *                                 CPU_ZERO(sb);
 *                                 CPU_SET(b * NUM_THREADS_PER_CPU, sb);
 *                                 assert(CPU_COUNT(sa) == 1);
 *                                 assert(CPU_COUNT(sb) == 1);
 *                                 printp("affinity is %d and %d (%d)\n", a, b,
 *                                        index);
 *                                 return;
 *                         } else {
 *                                 i++;
 *                         }
 *                 }
 *         }
 *
 *         assert(0);
 * }
 */

/*
 * Affinity:
 *
 * #define _GNU_SOURCE
 * #include <sched.h>
 *
 * pthread_attr_t attrs[2] = { 0 };
 *
 * pthread_attr_init(&attrs[0]);
 * pthread_attr_init(&attrs[1]);
 *
 * cpu_set_t cpusets[2];
 * affin_iter(&cpusets[0], &cpusets[1], k % affin_iter_max());
 *
 * pthread_attr_setaffinity_np(&attrs[0], sizeof(cpu_set_t),
 *                             &cpusets[0]);
 * pthread_attr_setaffinity_np(&attrs[1], sizeof(cpu_set_t),
 *                                               &cpusets[1]);
 */

static size_t pattern_size_in_trefi(struct pattern *p)
{
	if (p->multiblock.enabled) {
		assert(p->multiblock.num_refs);
		return p->multiblock.num_refs;
	} else {
		size_t sum = 0;

		for (size_t b = 0; b < p->num_blks; b++) {
			struct refblock *blk = p->blks[b].self;
			sum += blk->reps_of_blk;
		}

		assert(sum);

		return sum;
	}
}

static void hammer_postpone(struct pattern *p)
{
	double factor = 1;
	size_t num_trefi_hammer = pattern_num_trefi_hammer(p->num_trefi_factor);
	size_t times = num_trefi_hammer / pattern_size_in_trefi(p);

	for (size_t i = 0; i < (1UL << p->num_tries_hammer); i++) {
		struct timespec start = { 0 };
		struct timespec stop = { 0 };

		clock_gettime(CLOCK_MONOTONIC_RAW, &start);

		hammer_times(p, times);

		clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

		double real =
			(double)((1000000000 * (stop.tv_sec - start.tv_sec)) +
				 (stop.tv_nsec - start.tv_nsec)) /
			1000;

		if (i == 0) {
			factor = real / (num_trefi_hammer * 7.8);
			times = times / factor;
		}

		printp("%02lu, %12.2f us, %02lu, %4.2fx\n", i, real,
		       p->num_blks, real / (num_trefi_hammer * 7.8));

		/* Small random delay */
		{
			size_t min = 1UL << p->base_delay_trefi;
			size_t max = 1UL << (p->base_delay_trefi +
					     PATTERN_BASE_DELAY_WINDOW_POW);
			size_t n = rand() % (max - min) + min;
			struct timespec ts = { .tv_sec = 0,
					       .tv_nsec = n * 7800 };
			assert(!nanosleep(&ts, NULL));
		}
	}
}

static void chase_rotate(struct chase *c)
{
	size_t num_swaps_per_lane = c->misses_per_set - 1;
	size_t num_lanes = non_uniformity_to_lanes(c->non_uniformity);
	size_t lane_size = c->size / num_lanes;

	assert(lane_size == __WAYNESS);

	unsigned long *addrs = c->instance.addrs;
	bool *miss_at_index = c->miss_at_index;

	/* For each "lane" */
	for (size_t l = 0; l < num_lanes; l++) {
		size_t ns = 0;
		for (size_t a = 0; a < lane_size; a++) {
			size_t ai = l * lane_size + a;
			if (miss_at_index[ai]) {
				/* Because we minimize gaps! */
				for (size_t b = a + 1; b < lane_size; b++) {
					size_t bi = l * lane_size + b;
					if (miss_at_index[bi]) {
						unsigned long tmp = addrs[ai];
						addrs[ai] = addrs[bi];
						addrs[bi] = tmp;

						ns++;

						/* I.e. goto next_swap */
						break;
					}

					if (ns == num_swaps_per_lane) {
						goto next_lane;
					}
				}
			}
		}
	next_lane:
		assert(ns == num_swaps_per_lane);
	}
}

static void pattern_select_rotation(struct pattern *p)
{
	for (size_t i = 0; i < p->num_blks + 1; i++) {
		struct refblock *blk;

		if (i < p->num_blks) {
			blk = &p->blks[i];
		} else if (p->trigger.enabled &&
			   p->trigger.blk.cs[0].misses_per_set) {
			blk = &p->trigger.blk;
		} else {
			break;
		}

		if (blk->self != blk) continue;

		ssize_t r = -1;

		for (size_t j = 0; j < PATTERN_NUM_CHASES; j++) {
			struct chase *c = &blk->cs[j];

			if (!c->misses_per_set) continue;

			if (!j && blk->ct == EQUAL) {
				assert(r < 0);
				r = rand() % c->misses_per_set;
			}

			if (blk->ct != EQUAL) {
				c->rotation.r = rand() % c->misses_per_set;
			} else {
				assert(r >= 0);
				assert(r < c->misses_per_set);
				c->rotation.r = r;
			}

			for (size_t k = 0; k < c->rotation.r; k++) {
				chase_rotate(&blk->cs[j]);
			}
		}
	}
}

static void fuzzer_init(struct fuzzer *f, struct pattern *ps)
{
	f->state.mode = FUZZ;

	f->state.reproduce_index = 0;

	f->state.flips_total = 0;
	f->state.rounds_one_flip = 0;
	f->state.victim_rows = 0;
	f->state.tries = 0;

	f->fd = open("./flip.csv", O_RDWR | O_CREAT | O_TRUNC, __ALL_RW);
	assert(f->fd >= 0);

	f->seed_history = calloc(PATTERN_HISTORY, sizeof(size_t));
	assert(f->seed_history);
	f->seed_history_head = 0;
}

static char fuzzer_modec(struct fuzzer *f)
{
	if (f->state.mode == FUZZ) {
		return 'f';
	} else if (f->state.mode == REPRODUCE) {
		return 'r';
	} else if (f->state.mode == SWEEP) {
		return 's';
	} else {
		assert(0);
	}
}

static void pattern_select_data_pattern(struct pattern *p)
{
	size_t index = rand() % data_pattern_max();

	/* Same data pattern for all, for now */
	for (size_t i = 0; i < p->num_blks + 1; i++) {
		struct refblock *blk = &p->blks[i];

		if (i < p->num_blks) {
			blk = &p->blks[i];
		} else if (p->trigger.enabled) {
			blk = &p->trigger.blk;
		} else {
			break;
		}

		if (blk->self != blk) continue;

		data_pattern_iter(&blk->dp, index);
	}
}

static unsigned long lrand()
{
	assert(2 * sizeof(int) == sizeof(long));

	unsigned long y = rand();

	return (y << 8 * sizeof(int)) ^ rand();
}

#ifdef SPLIT_DETECT
static int split_detect(struct refblock *blk, struct refblock *trigger,
			struct llc_info *info, struct bank_stats *stats)
{
#define PATTERN_NUM_TIMINGS_SPLIT 2048
	refblock_install(blk);

	/*
	 * 1. Choose NOPs (x-axis)
	 * 2. Choose reps. per REF
	 * 3. Time (wall-clock) (y-axis)
	 * 4. Time (cycles) for (z-axis)
	 */
	int fd = open("./split.csv", O_RDWR | O_CREAT | O_APPEND, __ALL_RW);

	assert(fd >= 0);

	print_double_chase(blk, info, stats);

#define SPLIT_DETECT_MAX_NUM_REFS 1024
	double reps_to_refs[SPLIT_DETECT_MAX_NUM_REFS] = { 0 };
	size_t max_reps = 0;

	for (size_t reps = 1;; reps++) {
		double min = 0;
		size_t n = 0;

		printf("%02lu", reps);

		for (size_t i = 0; i < 1024; i++) {
			struct timespec start = { 0 };
			struct timespec stop = { 0 };

			unsigned long misses = 0;
			unsigned long cycles = 0;

			clock_gettime(CLOCK_MONOTONIC_RAW, &start);

			/*
			 * Always with one NOP. Looking for the minimum. This
			 * is multiblock! 
			 */
			asm_measure_single_block_no_nops(
				blk->cs[0].instance.addrs[0],
				blk->cs[1].instance.addrs[0], reps, 1, &misses,
				&cycles);

			clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

#ifdef KABY_LAKE
			if (misses ==
			    3 * reps * (4 * blk->cs[0].misses_per_set)) {
#else
			if (misses == reps * (4 * blk->cs[0].misses_per_set)) {
#endif
				double nanos =
					(double)((1000000000 * (stop.tv_sec -
								start.tv_sec)) +
						 (stop.tv_nsec -
						  start.tv_nsec));

				if (min == 0 || nanos < min) {
					min = nanos;
				}

				n++;
			}

			if (n >= 3) break;
		}

		/* Let's make sure we have at least two measurements */
		if (!(n > 1)) {
			printf(": %lu <= 1 (reliability)\n", n);
			close(fd);
			return 1;
		}

		assert(min);

		double num_refs = min / 7800;

		reps_to_refs[reps] = num_refs;

		if (num_refs > PATTERN_SYNC_MAX_REFS_MULTIBLOCK) {
			printf(": %.3f > %lu (size)\n", num_refs,
			       PATTERN_SYNC_MAX_REFS_MULTIBLOCK);
			assert(reps >= 2);
			max_reps = reps;
			break;
		} else {
			printf(": %.3f\n", num_refs);
		}

		assert(reps < SPLIT_DETECT_MAX_NUM_REFS);
	}

	printf("max. number of reps: %lu (%.3f)\n", max_reps,
	       reps_to_refs[max_reps]);

	/* 160 for 250, 400 for 100, 80 for 500, etc. (?) */
#define SPLIT_DETECT_MAX_NUM_NOPS_PER_CONS (4000 + 1)
	for (size_t nops_per = 1;
	     nops_per <= SPLIT_DETECT_MAX_NUM_NOPS_PER_CONS; nops_per += 10) {
		for (size_t reps = 1; reps <= max_reps; reps++) {
			printf("%05lu/%05d,%02lu/%02lu\n", nops_per,
			       SPLIT_DETECT_MAX_NUM_NOPS_PER_CONS, reps,
			       max_reps);

			double misses_array[PATTERN_NUM_TIMINGS_SPLIT] = { 0 };
			double cycles_array[PATTERN_NUM_TIMINGS_SPLIT] = { 0 };

			for (size_t i = 0; i < PATTERN_NUM_TIMINGS_SPLIT; i++) {
				unsigned long m = 0;
				unsigned long c = 0;

				asm_measure_single_block_no_nops(
					blk->cs[0].instance.addrs[0],
					blk->cs[1].instance.addrs[0], reps,
					nops_per, &m, &c);

				misses_array[i] = (double)m;
				cycles_array[i] = (double)c;
			}

			double misses = gsl_stats_median(
				misses_array, 1, PATTERN_NUM_TIMINGS_SPLIT);
			double cycles = gsl_stats_median(
				cycles_array, 1, PATTERN_NUM_TIMINGS_SPLIT);

			dprintf(fd, "%3lu,%3lu,%6.3f,%4lu,%4lu,%4lu,%6lu\n",
				reps, nops_per, reps_to_refs[reps],
				blk->cs[0].misses_per_set,
				reps * (4 * blk->cs[0].misses_per_set),
				(unsigned long)misses, (unsigned long)cycles);
		}
	}

	close(fd);

	return 0;
}
#endif

/*
 * Do a non-temporal write of the time it took to do the previous non-temporal
 * write. Into a random location of the array, sort later
 */
static void ref_detect(void)
{
#define VMA_SIZE (32 * (1UL << 30))
#define AMPLIFY 1
#define NUM_LOADS (1UL << 17)
#define NUM_TIMINGS (NUM_LOADS / AMPLIFY)
#define CACHE_LINE_SIZE 64

	/* 1. Allocate a huge amount of memory, several GBs */
	size_t num_cache_lines = VMA_SIZE / CACHE_LINE_SIZE;

	char *base = mmap(NULL, VMA_SIZE, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

	assert(base);

	int fd = open("./ref.csv", O_RDWR | O_CREAT, __ALL_RW);

	assert(fd >= 0);

	size_t *timings = calloc(NUM_TIMINGS, sizeof(size_t));

	assert(timings);

	srand(1);

	unsigned long *addrs = calloc(NUM_LOADS, sizeof(unsigned long));

	assert(addrs);

	unsigned long addr = base;

	/* Set up the pointer chase */
	for (size_t i = 0; i < NUM_LOADS; i++) {
	sample:;
		unsigned long next =
			(unsigned long)base +
			(rand() % num_cache_lines) * CACHE_LINE_SIZE;

		if (*(unsigned long *)next) goto sample;

		*(unsigned long *)addr = next;

		addrs[i] = addr;

		/* asm volatile("clflush (%0)\n\t" ::"r"(addr) : "memory"); */

		addr = next;
	}

	addr = (unsigned long)base;

	size_t prev = 0;
	size_t next = 1;

	struct timespec start = { 0 };
	struct timespec stop = { 0 };

	clock_gettime(CLOCK_MONOTONIC_RAW, &start);

	for (size_t i = 0; i < NUM_TIMINGS; i++) {
		asm volatile(
			"rdtsc\n\t"
			"lfence\n\t"

			/* next = next - prev */
			"mov %%rax, %[next]\n\t"
			"sub %[prev], %[next]\n\t"

			/*
				 * prev should become the old next, so that's new next
				 * + prev
				 */
			"add %[next], %[prev]\n\t"

			/* "mov (%[addr]), %[addr]\n\t" */
			"mov (%[addr]), %%r8\n\t"

			"mov %[next], (%[addr])\n\t"
			"mov %%r8, %[addr]\n\t"

			/* "mov (%[addr]), %[addr]\n\t" */
			/* "mov (%[addr]), %[addr]\n\t" */
			: [prev] "+r"(prev), [next] "+r"(next), [addr] "+r"(addr)
			:
			: "rax", "rdx", "r8");
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

	size_t nanos = ((1000000000 * (stop.tv_sec - start.tv_sec)) +
			(stop.tv_nsec - start.tv_nsec));

	size_t j = 0;

	for (size_t i = 0; i < NUM_LOADS; i++) {
		size_t y = *(size_t *)addrs[i];

		if (y >= (size_t)base) continue;

		assert(y);

		timings[j++] = y / AMPLIFY;
	}

	assert(j == NUM_TIMINGS);

	for (size_t i = 0; i < NUM_TIMINGS; i++) {
		dprintf(fd, "%lu,%lu,%lu\n", i, timings[i], nanos / 7800);
	}

	assert(0);
}

static void pool_to_pattern(struct eviction_set *pool,
			    struct eviction_set *rainbow, struct llc_info *info,
			    struct bank_stats *stats, struct pattern *ps)
{
	assert(pool);
	assert(rainbow);

	assert(pool_total_size(pool) / pool->size >= PATTERN_NUM_EVICTION_SETS);
	assert(pool_total_size(rainbow) / rainbow->size >=
	       PATTERN_RAINBOW_NUM_EVICTION_SETS);

	struct eviction_set **pss = calloc(PATTERN_NUM_EVICTION_SETS,
					   sizeof(struct eviction_set *));
	struct eviction_set **rss = calloc(PATTERN_RAINBOW_NUM_EVICTION_SETS,
					   sizeof(struct eviction_set *));

	assert(pss);
	assert(rss);

	{
		/* TODO: this should be moved into a function */
		size_t pool_size = 0;

		for (struct eviction_set *es = pool; es; es = es->next) {
			double misses;

			eviction_set_reorder(es, PATTERN_CHASE_HIGH_DENSITY,
					     info, stats);

			if (!fast_chase(es, info, stats,
					PATTERN_FAST_CHASE_MISSES, &misses,
					NULL)) {
				if (misses < PATTERN_FAST_CHASE_MISSES) {
					printf("got only %.2f misses after reorder\n",
					       misses);
					print_eviction_set(es, info, stats);
					assert(0);
				} else {
					printf("got %.2f misses after reorder (more than expected)\n",
					       misses);
				}
			}

			assert(pool_size < PATTERN_NUM_EVICTION_SETS);
			pss[pool_size++] = es;

			if (pool_size == PATTERN_NUM_EVICTION_SETS) break;

			if (es->next) {
				if (do_eviction_sets_overlap(es, es->next,
							     info)) {
					assert(0);
				}
			}
		}

		size_t i = 0;

		for (struct eviction_set *es = rainbow; es; es = es->next) {
			rss[i++] = es;

			if (i == PATTERN_RAINBOW_NUM_EVICTION_SETS) break;

			if (es->next) {
				if (do_eviction_sets_overlap(es, es->next,
							     info)) {
					assert(0);
				}
			}
		}
	}

	/*
	 * Check if there's something we need to reproduce first: go to sweep
	 * mode
	 */
	struct fuzzer f = { 0 };

	fuzzer_init(&f, ps);

	{
		struct pattern *p = NULL;
	fuzz:
		/* This is a bit hacky (TODO) */
		if (f.state.mode == SWEEP) {
#ifdef SLEDGEHAMMER
			goto __sledge_sweep;
#else
			goto sweep_sync;
#endif
		}

		if (p) free(p);
		p = pattern_pop(&f.ps);

		if (!p) {
			p = calloc(1, sizeof(struct pattern));
			assert(p);
			assert(pattern_iter_postpone(p));

			f.seed_history[f.seed_history_head] = p->seed;
			f.seed_history_head =
				(f.seed_history_head + 1) % PATTERN_HISTORY;
		} else {
			assert(f.state.mode == FUZZ);
			f.state.mode = SWEEP;
		}

#ifdef SLEDGEHAMMER
		{
		__sledge_sweep:
			struct eviction_set *es = pss[p->sledgehammer.set];
			sledge_set_to_pattern(p, es->addrs[rand() % es->size],
					      p->sledgehammer.sides,
					      p->sledgehammer.banks, stats);
			pattern_select_data_pattern(p);
		__sledge_reproduce:
			printf("%08lx:%c:sh: s:%04lu b:%02lu n:%02lu\n",
			       p->seed, fuzzer_modec(&f), p->sledgehammer.set,
			       p->sledgehammer.banks, p->sledgehammer.sides);
			pattern_chk_set_rows(p, false, NULL, stats);
			hammer_sledge(p, p->sledgehammer.banks);
			goto __sledge_check;
		}
#else
	sweep_sync:
		/* sweep_sync: only the first time */
		if (pattern_instantiate_chases(
			    p, p->rainbow ? rss : pss,
			    p->rainbow ? PATTERN_RAINBOW_NUM_EVICTION_SETS :
					       PATTERN_NUM_EVICTION_SETS,
			    info, stats)) {
			goto fuzz;
		}

#ifdef SPLIT_DETECT
		if (split_detect(&p->blks[0], NULL, info, stats)) {
			goto fuzz;
		}

		assert(0);
#endif

		pattern_install(p);

		if (p->multiblock.enabled) {
			if (pattern_sync(p, PATTERN_SYNC_HALVES)) goto fuzz;
		} else {
			for (size_t i = 0; i < p->num_blks; i++) {
				struct refblock *blk = &p->blks[i];

				if (blk->self != blk) continue;

				if (refblock_sync(blk)) goto fuzz;
			}
		}

	sweep:
		pattern_select_rotation(p);

		pattern_select_data_pattern(p);

	reproduce:
		if (p->trigger.enabled) {
			printf("%08lx:%c: %03lu/%03lu/%03lu %c/%c/%c/%c %1ld /%02ld:[(%02lu/%02lu)]",
			       p->seed, fuzzer_modec(&f), p->num_tries_hammer,
			       p->num_trefi_factor, p->base_delay_trefi,
			       p->rainbow ? 'r' : 's', p->gather ? 'g' : 's',
			       p->unique ? 'u' : 'r', p->interleave ? 'v' : 'h',
			       p->num_blks, p->trigger.blk.cs[0].instance.bank,
			       p->trigger.blk.cs[0].misses_per_set,
			       p->trigger.blk.cs[1].misses_per_set);
		} else {
			printf("%08lx:%c: %03lu/%03lu/%03lu %c/%c/%c/%c/%04lu %1ld",
			       p->seed, fuzzer_modec(&f), p->num_tries_hammer,
			       p->num_trefi_factor, p->base_delay_trefi,
			       p->rainbow ? 'r' : 's', p->gather ? 'g' : 's',
			       p->unique ? 'u' : 'r', p->interleave ? 'v' : 'h',
			       p->press_nops_per, p->num_blks);
		}

		for (size_t i = 0; i < p->num_blks; i++) {
			struct refblock *blk = p->blks[i].self;

			assert(blk->reps_per_ref);
			assert(blk->reps_per_ref %
				       (blk->__non_uniformity + 1) ==
			       0);

			if (p->multiblock.enabled) {
				printf(" [%02lu]%01lu/%02ld:[%02lux:(%02lu/%02lu)]",
				       blk->id, blk->__non_uniformity,
				       blk->cs[0].instance.bank,
				       blk->reps_per_ref,
				       blk->cs[0].misses_per_set,
				       blk->cs[1].misses_per_set);
			} else {
				assert(blk->cs[0].instance.bank ==
				       blk->cs[1].instance.bank);
				printf(" %06lu [%02lu]%01lu/%02ld:%02lux[%02lux:(%02lu/%02lu)]",
				       blk->nops_per_per_ref, blk->id,
				       blk->__non_uniformity,
				       blk->cs[0].instance.bank,
				       blk->reps_of_blk, blk->reps_per_ref,
				       blk->cs[0].misses_per_set,
				       blk->cs[1].misses_per_set);
			}
		}

		if (p->multiblock.enabled) {
			printf(" %6lu NOPs (+%4.2f/%4.2f R)\n",
			       p->multiblock.nops_per * SPLIT_DETECT_CONST,
			       p->multiblock.extra, p->multiblock.num_refs);
		} else {
			printf("\n");
		}

		pattern_chk_set_rows(p, false, NULL, stats);

		pattern_install(p);

		hammer_postpone(p);

#endif
		size_t num_flips;

		{
		__sledge_check:;
			size_t num_checked = 0;

			num_flips = pattern_chk_set_rows(p, true, &num_checked,
							 stats);

			if (f.state.mode != FUZZ) {
				f.state.flips_total += num_flips;
				f.state.rounds_one_flip += !!num_flips;
				f.state.victim_rows += num_checked;
				f.state.tries++;
			}
		}

		size_t actual_misses = pattern_verify_misses(p);
		size_t exp_misses = pattern_exp_misses(p);

		printf("%5lu/%5lu%s\n", actual_misses, exp_misses,
		       actual_misses < exp_misses ? " (!)" : "");

		/* For new flips, and at the end of a repro/sweep */
		if (f.state.mode == FUZZ ||
		    (f.state.mode == REPRODUCE &&
		     f.state.tries >= PATTERN_REPRODUCE_TRIES) ||
		    (f.state.mode == SWEEP &&
		     f.state.tries >= PATTERN_SWEEP_TRIES)) {
		sweep_abort:;
			time_t t = time(NULL);
			struct tm *tm = localtime(&t);

#ifdef SLEDGEHAMMER
			dprintf(f.fd,
				"%02d.%02d,%02d:%02d:%02d,%s,%08lx,%08lx,%08lx,%c,%03lu/%03lu/%03lu/%03lu/%03lu,%03ld,%02lu,%02lu,%02lu",
				tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
				tm->tm_min, tm->tm_sec, replay_info.snapshot,
				replay_info.pfn, replay_info.pre_seed, p->seed,
				fuzzer_modec(&f), f.state.flips_total,
				f.state.rounds_one_flip, f.state.victim_rows,
				f.state.tries, pattern_count_victim_rows(p),
				num_flips, p->sledgehammer.sides,
				p->sledgehammer.banks, p->sledgehammer.set);
#else
			dprintf(f.fd,
				"%02d.%02d,%02d:%02d:%02d,%s,%08lx,%08lx,%08lx,%c,%03lu/%03lu/%03lu/%03lu/%03lu,%03ld,%02ld,%01d,%06ld,%4.2f,%4.2f,%01d,%01d,%01d,%01d,%01d,%04lu,%03lu,%03lu,%03lu,%05lu,%05lu",
				tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
				tm->tm_min, tm->tm_sec, replay_info.snapshot,
				replay_info.pfn, replay_info.pre_seed, p->seed,
				fuzzer_modec(&f), f.state.flips_total,
				f.state.rounds_one_flip, f.state.victim_rows,
				f.state.tries, pattern_count_victim_rows(p),
				num_flips, p->num_blks, p->multiblock.enabled,
				p->multiblock.nops_per, p->multiblock.extra,
				p->multiblock.num_refs, p->trigger.enabled,
				p->gather, p->unique, p->interleave, p->rainbow,
				p->press_nops_per, p->num_tries_hammer,
				p->num_trefi_factor, p->base_delay_trefi,
				actual_misses, exp_misses);
#endif
			/* History */
			for (size_t i = 0; i < PATTERN_HISTORY; i++) {
				ssize_t j = ((f.seed_history_head - i - 1) +
					     PATTERN_HISTORY) %
					    PATTERN_HISTORY;
				dprintf(f.fd, ";%08lx", f.seed_history[j]);
			}
#ifndef SLEDGEHAMMER
			for (size_t i = 0; i < p->num_blks; i++) {
				struct refblock *blk = p->blks[i].self;

				for (size_t j = 0; j < PATTERN_NUM_CHASES;
				     j++) {
					struct chase *c = &blk->cs[j];

					dprintf(f.fd,
						";[%02ld],%03lu,%02lu,%01d,%04lu,%02lu,%02lu,%02ld,%01lu",
						blk->id, blk->num_flips,
						c->misses_per_set, c->gather,
						blk->nops_per_per_ref,
						blk->reps_per_ref,
						blk->reps_of_blk,
						c->instance.bank,
						blk->__non_uniformity);

					bool had_miss = false;

					for (size_t k = 0; k < c->size; k++) {
						if (c->miss_at_index[k]) {
							dprintf(f.fd, "%c%05lu",
								had_miss ? ',' :
										 ':',
								c->flips_around_index
									[k]);
							had_miss = true;
						}
					}
				}

				if (f.state.mode == FUZZ && num_flips) {
					print_double_chase(blk, info, stats);
				}
			}

			/* Reset */
			for (size_t i = 0; i < p->num_blks + 1; i++) {
				struct refblock *blk;

				if (i < p->num_blks) {
					blk = &p->blks[i];
				} else if (p->trigger.enabled &&
					   p->trigger.blk.cs[0].misses_per_set) {
					blk = &p->trigger.blk;
				} else {
					break;
				}

				if (blk->self != blk) continue;

				blk->num_flips = 0;

				for (size_t j = 0; j < PATTERN_NUM_CHASES;
				     j++) {
					memset(blk->cs[j].flips_around_index, 0,
					       sizeof(size_t) *
						       blk->cs[j].size);
				}
			}
#endif
			dprintf(f.fd, "\n");

			if (f.state.mode == FUZZ && num_flips) {
				f.state.mode = REPRODUCE;
				assert(!f.state.reproduce_index);
#ifdef __EXPERIMENT_HC_REPRODUCE
				assert(!pattern_hc_next_const_time(
					p, f.state.reproduce_index));
#endif
			} else if (f.state.mode == REPRODUCE) {
#ifdef __EXPERIMENT_HC_REPRODUCE
				f.state.reproduce_index++;

				if (pattern_hc_next_const_time(
					    p, f.state.reproduce_index)) {
					f.state.mode = SWEEP;
					f.state.reproduce_index = 0;

					/* Restore */
					pattern_hc_next_const_time(p,
								   p->hc_index);
				}
#else
				f.state.mode = SWEEP;
#endif
			} else if (f.state.mode == SWEEP) {
				f.state.mode = FUZZ;
			}

			f.state.flips_total = 0;
			f.state.rounds_one_flip = 0;
			f.state.victim_rows = 0;
			f.state.tries = 0;
		}

		if (f.state.mode != FUZZ) {
			if (f.state.mode == REPRODUCE) {
#ifdef SLEDGEHAMMER
				goto __sledge_reproduce;
#else
				goto reproduce;
#endif
			} else if (f.state.mode == SWEEP) {
#ifdef SLEDGEHAMMER
				goto __sledge_sweep;
#else
				for (size_t i = 0;
				     i < PATTERN_NUM_TRIES_REINSTANTIATE; i++) {
					if (!pattern_instantiate_chases(
						    p, p->rainbow ? rss : pss,
						    p->rainbow ?
							    PATTERN_RAINBOW_NUM_EVICTION_SETS :
								  PATTERN_NUM_EVICTION_SETS,
						    info, stats)) {
						goto sweep;
					}
				}

				goto sweep_abort;
#endif
			}
		} else {
			goto fuzz;
		}
	}
}

/*
 * Per-block sync
 * Double-sided only
 * Same bank only
 * Given number of NOPs
 *
 * parser-id,num-blocks,reps-b1,nops-b1,per-ref-reps-b1,misses-b1,etc.
 *
 * Example: 000:2;4,8451,11,02;15,8251,03,10
 *
 * 04x:[8451 NOPs @ 11x:(02)]---15x:[8251 NOPs @ 03x:(10)]
 */
void parse_simple(char *line, size_t size, struct pattern *out)
{
	size_t field = 0;
	size_t head = 0;
	size_t tail = 0;

	for (size_t i = 0; i < size; i++) {
		if (line[i] == ',' || line[i] == ';' || line[i] == '\n') {
			int y = atoi(&line[tail]);

			/* printf("field %lu: %d (%s)\n", field, y, &line[tail]); */

			if (field == 0) {
				assert(y);
				out->num_blks = y;
			} else {
				/* -1 for num_blks */
				size_t j = (field - 1) / 4;
				size_t k = (field - 1) % 4;

				if (k == 0) {
					out->blks[j].self = &out->blks[j];
					out->blks[j].id = j;
					out->blks[j].ct = EQUAL;

					out->blks[j].reps_of_blk = y;
				} else if (k == 1) {
					assert(y >= SPLIT_DETECT_CONST);
					out->blks[j].nops_per_per_ref =
						y / SPLIT_DETECT_CONST;
				} else if (k == 2) {
					out->blks[j].reps_per_ref = y;
				} else if (k == 3) {
					assert(PATTERN_NUM_CHASES >= 2);

					for (size_t l = 0; l < 2; l++) {
						out->blks[j]
							.cs[l]
							.misses_per_set = y;
						out->blks[j].cs[l].hit_reps = 1;
					}
				} else {
					assert(0);
				}
			}

			/* +1 to skip the ,/; */
			tail = head + 1;
			field++;
		}

		head++;

		if (line[i] == '\n') break;
	}

	out->multiblock.enabled = false;
	out->trigger.enabled = false;
	out->gather = false;
	out->unique = true;
}

static struct pattern *fuzzer_import_pat_info(const char *file)
{
	if (!strcmp(file, "null")) return NULL;

#define PARSER_ID_LEN 3
	FILE *stream = fopen(file, "r");
	char *lineptr = NULL;
	size_t n = 0;
	size_t l = 0;

	static const void (*parsers[])(char *, size_t,
				       struct pattern *) = { parse_simple };

	assert(stream);

	struct pattern *ps = NULL;

	for (;;) {
		ssize_t m = getline(&lineptr, &n, stream);

		if (m == -1) break;

		/* At least... 001:\n" */
		assert(m >= PARSER_ID_LEN + 2);

		/* m - 1 because otherwise we get '\n' */
		for (size_t i = 0; i < m - 1; i++) {
			int id = atoi(&lineptr[0]);

			assert(id <= sizeof(parsers) / sizeof(void *));

			struct pattern *p = pattern_new(&ps);

			parsers[id](&lineptr[PARSER_ID_LEN + 1], m, p);

			break;
		}

		l++;
	}

	free(lineptr);

	return ps;
}

/* Number of sets added to pool */
static int augment_rainbow(struct eviction_set **rainbow,
			   struct eviciton_set *es, struct eviction_set *fs,
			   struct llc_info *info, struct bank_stats *stats)
{
	struct eviction_set *x =
		eviction_set_change_bank(es, -1, info, stats, *rainbow);

	if (!x) return 0;

	struct eviction_set *y =
		eviction_set_change_bank(fs, -1, info, stats, *rainbow);

	if (!y) return 0;

	if (!pool_add_eviction_sets(rainbow, x, y, info)) {
		print_eviction_set(x, info, stats);
		print_eviction_set(y, info, stats);
		return 2;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct args args = { 0 };
	struct bank_stats stats = { 0 };

#ifndef SPLIT_DETECT
	assert(PATTERN_NUM_EVICTION_SETS >= PATTERN_NUM_EVICTION_SETS_MIN);
#endif

	setbuf(stdout, NULL);

	argparse(argc, argv, &args);

	size_t seed = time(NULL);

	/* NOTE: put the pre-seed here! */
#ifdef __TARGET_SEED
	seed = 0x662a6b83;
#endif

	printp("pre-seed is %lu\n", seed);

	assert(strlen(replay_info.snapshot));

	srand(seed);

	replay_info.pre_seed = seed;
	assert(!(0xffffffff00000000UL & replay_info.pre_seed));

	/* nop_sync(); */

	drama_import_bank_stats_from_file(args.bank_stats_file, false, &stats);
	replay_info.pfn = stats.pfn;

	/* ref_detect(); */

	struct pattern *ps = fuzzer_import_pat_info(args.pat_info_file);

	struct eviction_set *root =
		new_root_eviction_set(stats.base, (1UL << 30) - 1, &cn104);

	/* 3x wayness for non-uniformity! */
	struct eviction_set *larger = eviction_set_expand(
		root, PATTERN_NUM_LANES_PER_EVICTION_SET * cn104.wayness,
		(1UL << 30) - 1, &cn104, &stats);

	size_t num_banks = count_banks(&stats);

	size_t pool_size = 0;
	size_t rainbow_size = 0;

	struct eviction_set *pool = NULL;
	struct eviction_set *rainbow = NULL;

	for (size_t i = 0; i < 1UL << cn104.num_set_bits; i++) {
		size_t b = rand() % num_banks;
		size_t j = i;

		/* 1. Change the set */
		struct eviction_set *es =
			fork_eviction_set_with_new_set(larger, j, &cn104);

		/*
		 * 2. Restore eviction: fork_eviction_set_with_new_set does not
		 * try, just sets the set
		 */
		struct eviction_set *fs = eviction_set_sweep_row_bits(
			es, (1UL << 30) - 1, &cn104, &stats);
		free_eviction_set(es);

		if (!fs) {
			printp("could not restore eviction\n");
			continue;
		}

		/*
		 * 3. Set the bank; this function does try to preserve eviction
		 * by changing row bits
		 */
		struct eviction_set *esb =
			eviction_set_change_bank(fs, b, &cn104, &stats, pool);
		free_eviction_set(fs);

		if (!esb) {
			printp("could not change bank\n");
			continue;
		}

		/*
		 * 4. Our set and bank are correct. Let's find the counter set
		 */
		struct eviction_set *fsb =
			find_counter_set(esb, esb->addrs[0], &cn104, &stats);

		if (!fsb) {
			free_eviction_set(esb);
			printp("could not find counter set\n");
			continue;
		} else {
			struct eviction_set *gsb = NULL;
			size_t c = b;

			while (!gsb) {
				c = (c + 1) % num_banks;
				gsb = eviction_set_change_bank(esb, c, &cn104,
							       &stats, pool);
			}

			struct eviction_set *hsb = find_counter_set(
				gsb, gsb->addrs[0], &cn104, &stats);

			if (!hsb) {
				free_eviction_set(gsb);
				continue;
			}

			if (!pool_add_eviction_sets(&pool, gsb, hsb, &cn104)) {
				print_eviction_set(gsb, &cn104, &stats);
				print_eviction_set(hsb, &cn104, &stats);
				pool_size += 2;

				/* 
				 * Might still fail, I think, in that case we
				 * should also remove the two eviction sets
				 * above...
				 */
				rainbow_size += augment_rainbow(
					&rainbow, gsb, hsb, &cn104, &stats);
			}
		}

		if (!pool_add_eviction_sets(&pool, esb, fsb, &cn104)) {
			print_eviction_set(esb, &cn104, &stats);
			print_eviction_set(fsb, &cn104, &stats);
			pool_size += 2;
			rainbow_size += augment_rainbow(&rainbow, esb, fsb,
							&cn104, &stats);

			if (pool_size >= PATTERN_NUM_EVICTION_SETS &&
			    rainbow_size >= PATTERN_RAINBOW_NUM_EVICTION_SETS) {
				break;
			}
		}

		printp("bank %lu/%lu, set %lu, eviction sets %lu/%lu %lu/%lu\n",
		       b, num_banks, i, pool_size, PATTERN_NUM_EVICTION_SETS,
		       rainbow_size, PATTERN_RAINBOW_NUM_EVICTION_SETS);
	}

	assert(pool);
	assert(rainbow);
	assert(pool_size >= PATTERN_NUM_EVICTION_SETS);
	assert(rainbow_size >= PATTERN_RAINBOW_NUM_EVICTION_SETS);

	pool_to_pattern(pool, rainbow, &cn104, &stats, ps);
}
