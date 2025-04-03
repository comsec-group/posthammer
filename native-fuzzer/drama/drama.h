#define DRAMA_MAX_NUM_BANKS 256
#define DRAMA_MAX_NUM_FUNCTIONS 8
#define DRAMA_NUM_ADDRS_PER_BANK 16
#define DRAMA_ROW_MISS_GAP_MIN 100
#define DRAMA_ROW_MISS_CLOSER_TO_FACTOR 2

/* Max. number of bits we consider for the DRAM address */
#define DRAMA_MAX_NUM_BITS 30

#define read_pair_into_t(first, second, t)                                     \
	asm volatile("mfence\n\t" /* globally visible loads and stores */      \
		     "lfence\n\t" /* mfence and anything else done */          \
		     "rdtsc\n\t"                                               \
		     "lfence\n\t" /* rdtsc done */                             \
		     "mov %%rax, %%rbx\n\t"                                    \
                                                                               \
		     "mov (%1), %%r8\n\t"                                      \
		     "mov (%2), %%r9\n\t"                                      \
                                                                               \
		     "mfence\n\t" /* globally visible loads and stores */      \
		     "lfence\n\t" /* local instructions done */                \
		     "rdtsc\n\t"                                               \
		     "lfence\n\t" /* rdtsc done */                             \
		     "sub %%rbx, %%rax\n\t"                                    \
		     : "+a"(t)                                                 \
		     : "r"(first), "r"(second)                                 \
		     : "rbx", "rdx", "r8", "r9", "memory");

struct bank_stats {
	int fd;

	unsigned long base;
	unsigned long pfn;
	unsigned long offby;

	double row_hit_median;
	double row_hit_below;
	double row_miss_above;

	size_t bank_bit_ids[DRAMA_MAX_NUM_BITS];
	size_t bank_functions[DRAMA_MAX_NUM_FUNCTIONS];

	unsigned long row_mask;
};

void drama_import_bank_stats_from_file(const char *file, bool collect,
				       struct bank_stats *stats);

double sort_print_quantiles_std_median(double *timings, size_t num_timings);
double read_pair_min(unsigned long x, unsigned long y, unsigned long offby);
int bank_stats_was_miss(struct bank_stats *stats, double t);

unsigned bank_is(unsigned long addr, unsigned long *bank_functions);
unsigned long prep_for_bank_preserving_xor(unsigned long bank_function,
					   unsigned long xor_mask,
					   unsigned long x);
