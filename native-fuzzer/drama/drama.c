#include <assert.h>
#include <gsl/gsl_sort.h>
#include <gsl/gsl_statistics.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../mem/mem.h"
#include "drama.h"

#define DRAMA_NUM_PAIRS 10000
#define DRAMA_NUM_TIMINGS 10000

#define BANK_BITS_CSV "bank-bits"
#define BANK_FUNCTIONS_CSV "bank-functions"
#define ROW_MASK_CSV "row-mask"

#define QUANTILES_STD_NUM 13
#define QUANTILES_STD                                                          \
	0, 0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90, 0.95, 0.99, 1

/*
 * AT&T: source -> dest
 *
 * rax is generally not a clobber because it is output by rdtsc (+a)
 */

#define read_calibrate(t)                                                      \
	asm volatile("mfence\n\t" /* globally visible loads and stores */      \
		     "lfence\n\t" /* mfence and anything else done */          \
		     "rdtsc\n\t"                                               \
		     "lfence\n\t" /* rdtsc done */                             \
		     "mov %%rax, %%rbx\n\t"                                    \
                                                                               \
		     "mfence\n\t" /* globally visible loads and stores */      \
		     "lfence\n\t" /* local instructions done */                \
		     "rdtsc\n\t"                                               \
		     "lfence\n\t" /* rdtsc done */                             \
		     "sub %%rbx, %%rax\n\t"                                    \
		     : "+a"(t)                                                 \
		     :                                                         \
		     : "rbx", "rdx");

struct drama {
	struct bank_stats stats;

	/*
	 * Each row contains DRAMA_NUM_ADDRS_PER_BANK. Each columns contains
	 * DRAMA_MAX_NUM_BANKS
	 *
	 * 0: .....
	 * 1: .....
	 * 2: .....
	 */
	unsigned long
		bank_matrix[DRAMA_NUM_ADDRS_PER_BANK * DRAMA_MAX_NUM_BANKS];
	double bank_bit_counts[DRAMA_MAX_NUM_BITS];
};

static void __sort_print_quantiles(double *timings, size_t num_timings,
				   const double *quantiles,
				   size_t num_quantiles, double *values)
{
	gsl_sort(timings, 1, num_timings);

	for (size_t i = 0; i < num_quantiles; i++) {
		values[i] = gsl_stats_quantile_from_sorted_data(
			timings, 1, num_timings, quantiles[i]);

		printf("%0.2f: %.2f", quantiles[i], values[i]);

		if (quantiles[i] == 0.50) {
			printf(" <--\n");
		} else {
			printf("\n");
		}
	}
}

double sort_print_quantiles_std_median(double *timings, size_t num_timings)
{
	static double quantiles[] = { QUANTILES_STD };
	double values[QUANTILES_STD_NUM] = { 0 };

	__sort_print_quantiles(timings, num_timings, quantiles,
			       QUANTILES_STD_NUM, values);

	return values[5];
}

#define CALIBRATE_NUM_TIMINGS 10000
static double calibrate(void)
{
	double *timings = calloc(CALIBRATE_NUM_TIMINGS, sizeof(double));

	for (size_t i = 0; i < CALIBRATE_NUM_TIMINGS; i++) {
		unsigned long t = 0;
		read_calibrate(t);
		timings[i] = t;
	}

	printf("Measurement overhead:\n");

	return sort_print_quantiles_std_median(timings, CALIBRATE_NUM_TIMINGS);
}

/*
 * Return 1 if a miss, 0 if not, 2 if undecided
 */
int bank_stats_was_miss(struct bank_stats *stats, double t)
{
	if (t <= stats->row_hit_below) {
		/* Certainly a hit */
		return 0;
	} else if (t >= stats->row_miss_above) {
		/* Certainly a miss */
		return 1;
	} else {
		double dh = t - stats->row_hit_below;
		double dm = stats->row_miss_above - t;

		if (DRAMA_ROW_MISS_CLOSER_TO_FACTOR * dh < dm) {
			/*
			 * printf("Hit: [%.0f]--%.0f--[%.0f]--%.0f--[%.0f]\n",
			 *        stats->row_hit_below, dh, t, dm,
			 *        stats->row_miss_above);
			 */
			return 0;
		} else if (DRAMA_ROW_MISS_CLOSER_TO_FACTOR * dm < dh) {
			/*
			 * printf("Miss: [%.0f]--%.0f--[%.0f]--%.0f--[%.0f]\n",
			 *        stats->row_hit_below, dh, t, dm,
			 *        stats->row_miss_above);
			 */
			return 1;
		} else {
			/*
			 * printf("Undecided: [%.0f]--%.0f--[%.0f]--%.0f--[%.0f]\n",
			 *        stats->row_hit_below, dh, t, dm,
			 *        stats->row_miss_above);
			 */
			return 2;
		}
	}
}

static int matrix_add_to_bank(unsigned long *bank_matrix, size_t bank_id,
			      unsigned long addr)
{
	assert(bank_id < DRAMA_MAX_NUM_BANKS);

	for (size_t i = 0; i < DRAMA_NUM_ADDRS_PER_BANK; i++) {
		unsigned long *slot =
			&bank_matrix[bank_id * DRAMA_NUM_ADDRS_PER_BANK + i];

		if (!*slot) {
			*slot = addr;
			return 0;
		}
	}

	/* Full! */
	return -1;
}

static unsigned long matrix_get_from_bank(unsigned long *bank_matrix,
					  size_t bank_id, size_t i)
{
	assert(bank_id < DRAMA_MAX_NUM_BANKS);
	return bank_matrix[bank_id * DRAMA_NUM_ADDRS_PER_BANK + i];
}

static void matrix_print_counts(unsigned long *bank_matrix)
{
	size_t total = 0;
	size_t num_banks = 0;

	for (size_t b = 0; b < DRAMA_MAX_NUM_BANKS; b++) {
		size_t count = 0;

		for (size_t i = 0; i < DRAMA_NUM_ADDRS_PER_BANK; i++) {
			if (bank_matrix[b * DRAMA_NUM_ADDRS_PER_BANK + i]) {
				count++;
			}
		}

		total += count;

		if (count) num_banks++;

		printf("Bank %lu: %lu\n", b, count);
	}

	printf("Total number of addresses is %lu. Number of banks %lu. Expect %lu per bank\n",
	       total, num_banks, total / num_banks);
}

double read_pair_min(unsigned long x, unsigned long y, unsigned long offby)
{
	double timings[DRAMA_NUM_TIMINGS] = { 0 };

	for (size_t i = 0; i < DRAMA_NUM_TIMINGS; i++) {
		/* Must be an unsigned long! */
		unsigned long t = 0;

		asm volatile("clflushopt (%0)" ::"r"(x) : "memory");
		asm volatile("clflushopt (%0)" ::"r"(y) : "memory");

		read_pair_into_t(x, y, t);

		timings[i] = t > offby ? t - offby : 0;
	}

	return gsl_stats_min(timings, 1, DRAMA_NUM_TIMINGS);
}

static void detect_banks(struct drama *drama)
{
	size_t ids[2 * DRAMA_NUM_PAIRS] = { 0 };
	size_t num_addrs = (DRAMA_NUM_ADDRS_PER_BANK * DRAMA_MAX_NUM_BANKS) / 4;
	struct bank_stats *stats = &drama->stats;

	/* Stats need to have been initialized by collect_bank_stats */
	assert(stats->row_hit_median < stats->row_miss_above);

	for (size_t i = 0; i < num_addrs; i++) {
		/* 1 GB minus 5 for the 64-byte cache lines */
		ids[i] = rand() % (1UL << (30 - 5));
	}

	for (size_t i = 0; i < num_addrs;) {
		unsigned long new = stats->base + (ids[i] << 5);

		for (size_t b = 0; b < DRAMA_MAX_NUM_BANKS; b++) {
			unsigned long old =
				matrix_get_from_bank(drama->bank_matrix, b, 0);

			if (!old) {
				assert(!matrix_add_to_bank(drama->bank_matrix,
							   b, new));
				goto next;
			} else {
				double min =
					read_pair_min(old, new, stats->offby);
				int miss = bank_stats_was_miss(stats, min);

				if (miss == 1) {
					if (!matrix_add_to_bank(
						    drama->bank_matrix, b,
						    new)) {
						printf("%lu: Added 0x%lx to bank/row %lu\n",
						       100 * i / num_addrs, new,
						       b);
					} else {
						printf("0x%lx maps to bank %lu, which is full\n",
						       new, b);
					}

					goto next;
				} else if (miss == 2) {
					/* Something's off: don't add it */
					goto next;
				} /* else: a hit, keep searching */
			}
		}

		printf("Couldn't find a bank for 0x%lx\n", new);
		assert(0);
	next:
		i++;
	}

	matrix_print_counts(drama->bank_matrix);
}

static void export_bank_bits(struct drama *drama)
{
	double tmp[DRAMA_MAX_NUM_BITS] = { 0 };

	assert(drama->stats.fd);

	memcpy(tmp, drama->bank_bit_counts,
	       sizeof(double) * DRAMA_MAX_NUM_BITS);

	double max = gsl_stats_max(tmp, 1, DRAMA_MAX_NUM_BITS);

	printf("\nLikelihood vector:\n");

	for (size_t i = 0; i < DRAMA_MAX_NUM_BITS; i++) {
		printf("Bit %lu: %.0f\n", i, drama->bank_bit_counts[i]);
	}

	dprintf(drama->stats.fd, BANK_BITS_CSV);

	for (size_t i = 0, j = 0; i < DRAMA_MAX_NUM_BITS; i++) {
		if (drama->bank_bit_counts[i] >= max * 0.95) {
			drama->stats.bank_bit_ids[j++] = i;
			dprintf(drama->stats.fd, ",%02lu", i);
		}
	}

	dprintf(drama->stats.fd, "\n");
}

static void find_bank_bits(struct drama *drama)
{
	struct bank_stats *stats = &drama->stats;

	for (size_t b = 0; b < DRAMA_MAX_NUM_BANKS; b++) {
		printf("\rSifting through banks... %lu %%",
		       100 * b / DRAMA_MAX_NUM_BANKS);

		for (size_t i = 0; i < DRAMA_NUM_ADDRS_PER_BANK; i += 2) {
			unsigned long first =
				matrix_get_from_bank(drama->bank_matrix, b, i);
			unsigned long second = matrix_get_from_bank(
				drama->bank_matrix, b, i + 1);

			if (!(first && second)) {
				continue;
			} else {
				double min_should_miss = read_pair_min(
					first, second, stats->offby);

				if (bank_stats_was_miss(stats,
							min_should_miss) != 1) {
					continue;
				} else {
					for (size_t shift = 6;
					     shift < DRAMA_MAX_NUM_BITS;
					     shift++) {
						unsigned long mutation =
							first ^ (1UL << shift);
						double median = read_pair_min(
							mutation, second,
							stats->offby);

						if (bank_stats_was_miss(
							    stats, median) ==
						    0) {
							/*
							 * No longer in the
							 * same bank:
							 * significant
							 *
							 * If we change a row
							 * bit, we should still
							 * get a miss: we
							 * were going to
							 * different rows
							 * anyway, the chance
							 * that we mutated the
							 * row to get the same
							 * one is very small.
							 * Will show up in the
							 * likelihood vector
							 */
							drama->bank_bit_counts
								[shift]++;
						}
					}
				}
			}
		}
	}
}

static unsigned apply_function_as_mask(unsigned long function_as_mask,
				       unsigned long x)
{
	unsigned long z = x & function_as_mask;
	unsigned y = 0;

	while (z) {
		y = y ^ (z & 0x1);
		z = z >> 1;
	}

	assert(y == 0 || y == 1);

	return y;
}

unsigned bank_is(unsigned long addr, unsigned long *bank_functions)
{
	unsigned bank = 0;
	size_t j = 0;

	for (ssize_t i = DRAMA_MAX_NUM_FUNCTIONS - 1; i >= 0; i--) {
		if (bank_functions[i]) {
			bank ^= apply_function_as_mask(bank_functions[i], addr);
			bank = bank << 1;
			j++;
		}
	}

	bank = bank >> 1;

	assert(bank < (1UL << j));

	return bank;
}

static int try_candidate(unsigned long *bank_matrix,
			 unsigned long function_as_mask)
{
	for (size_t b = 0; b < DRAMA_MAX_NUM_BANKS; b++) {
		unsigned long first = matrix_get_from_bank(bank_matrix, b, 0);

		/* Must be at least two banks... */
		assert(!(b < 2 && !first));

		if (!first) continue;

		unsigned y = apply_function_as_mask(function_as_mask, first);

		for (size_t i = 1; i < DRAMA_NUM_ADDRS_PER_BANK; i++) {
			unsigned long other =
				matrix_get_from_bank(bank_matrix, b, i);

			if (!other) continue;

			if (apply_function_as_mask(function_as_mask, other) !=
			    y) {
				return 0;
			}
		}
	}

	return 1;
}

static void export_and_find_bank_functions(struct drama *drama)
{
	size_t num_bank_bits = 0;
	size_t *bank_bit_ids = drama->stats.bank_bit_ids;

	assert(drama->stats.fd);

	for (size_t i = 0; i < DRAMA_MAX_NUM_BITS; i++) {
		if (bank_bit_ids[i]) num_bank_bits++;
	}

	assert(num_bank_bits % 2 == 0);

	dprintf(drama->stats.fd, BANK_FUNCTIONS_CSV);

	for (size_t i = 0, k = 0; i < num_bank_bits; i++) {
		for (size_t j = i + 1; j < num_bank_bits; j++) {
			assert(i != j);
			unsigned long mask = (1UL << bank_bit_ids[i]) ^
					     (1UL << bank_bit_ids[j]);

			if (try_candidate(drama->bank_matrix, mask)) {
				assert(k < DRAMA_MAX_NUM_FUNCTIONS);
				drama->stats.bank_functions[k++] = mask;
				dprintf(drama->stats.fd, ",0x%016lx", mask);
			}
		}
	}

	dprintf(drama->stats.fd, "\n");
}

unsigned long prep_for_bank_preserving_xor(unsigned long bank_function,
					   unsigned long xor_mask,
					   unsigned long x)
{
	/* Both were flipped: fine */
	if ((bank_function & xor_mask) == bank_function) {
		return x;
	} else if (bank_function & xor_mask) {
		/* One was flipped: compensate */
		unsigned long compensation = ~xor_mask & bank_function;

		assert(compensation != bank_function);
		assert((compensation ^ (bank_function & xor_mask)) ==
		       bank_function);

		return x ^ compensation;
	} else {
		/* None were flipped: fine */
		return x;
	}
}

static void export_and_find_row_mask(struct drama *drama)
{
	unsigned long one = matrix_get_from_bank(drama->bank_matrix, 0, 0);
	unsigned long two = matrix_get_from_bank(drama->bank_matrix, 0, 1);

	assert(one);
	assert(two);

	unsigned bank = bank_is(one, drama->stats.bank_functions);

	assert(bank_is(two, drama->stats.bank_functions) == bank);
	assert(bank_stats_was_miss(&drama->stats,
				   read_pair_min(one, two,
						 drama->stats.offby)) == 1);

	for (size_t j = 1; j <= 30; j++) {
		/* As soon as we hit same row, done! */
		unsigned long row_mask = ((1UL << j) - 1) << (30 - j);
		unsigned long xor_mask = (row_mask & one) ^ (two & row_mask);
		unsigned long three = two;

		/* Did we change the bank? Undo it */
		for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
			unsigned long bank_function =
				drama->stats.bank_functions[i];

			if (bank_function) {
				three = prep_for_bank_preserving_xor(
					bank_function, xor_mask, three);
			}
		}

		/* Give two the row mask of one */
		three = three ^ xor_mask;

		assert(bank_is(three, drama->stats.bank_functions) == bank);

		double median = read_pair_min(one, three, drama->stats.offby);

		printf("%lu,0x%lx,0x%lx,0x%lx,0x%lx,%.2f\n", j, one, three,
		       row_mask, xor_mask, median);

		/* 
		 * If this fails, the row masks are equal: premise of our
		 * experiment is that they are different
		 */
		if (bank_stats_was_miss(&drama->stats, median) == 0) {
			/* Same bank, same row: we found the row mask */
			assert(xor_mask);
			drama->stats.row_mask = row_mask;
			dprintf(drama->stats.fd, ROW_MASK_CSV);
			dprintf(drama->stats.fd, ",0x%016lx\n", row_mask);
			return;
		}
	}

	assert(0);
}

static void collect_bank_stats(struct bank_stats *stats)
{
	size_t ids[2 * DRAMA_NUM_PAIRS] = { 0 };
	double mins[DRAMA_NUM_PAIRS] = { 0 };

	for (size_t i = 0; i < 2 * DRAMA_NUM_PAIRS; i++) {
		/* 1 GB minus 5 for the 64-byte cache lines */
		ids[i] = rand() % (1UL << (30 - 5));
	}

	for (size_t i = 0; i < 2 * DRAMA_NUM_PAIRS; i += 2) {
		unsigned long first = stats->base + (ids[i] << 5);
		unsigned long second = stats->base + (ids[i + 1] << 5);

		mins[i / 2] = read_pair_min(first, second, stats->offby);
	}

	static double quantiles[] = { QUANTILES_STD };
	double values[QUANTILES_STD_NUM] = { 0 };

	printf("Row hits and misses:\n");
	__sort_print_quantiles(mins, DRAMA_NUM_PAIRS, quantiles,
			       QUANTILES_STD_NUM, values);

	stats->row_hit_median = values[5]; /* Median */

	for (int i = QUANTILES_STD_NUM - 1; i >= 0; i--) {
		if (values[i] - values[i - 1] >= DRAMA_ROW_MISS_GAP_MIN) {
			stats->row_miss_above = values[i];
			stats->row_hit_below = values[i - 1];
			assert((i != 5) && ((i - 1) != 5));
			goto done;
		}
	}

	/* Could not find threshold */
	assert(0);
done:
	printf("Row hits below %.0f. Misses above %.0f.\n",
	       stats->row_hit_below, stats->row_miss_above);
}

struct args {
	char *hostname;
	/* sudo lshw -c memory | grep product */
	char *mem_product;
	/* sudo lshw -c memory | grep vendor | grep -Ev "Intel|NO DIMM" */
	char *mem_vendor;

	char *bank_stats_file;
};

static void argparse(int argc, char **argv, struct args *args)
{
	/*
	 * 0: drama
	 *
	 * 1: Hostname
	 * 2: Mem. product: some serial number to identify the DIMM
	 * 3: Mem. vendor: Kingston, Samsung, etc.
	 */
	assert(argc == 4);

	args->hostname = argv[1];
	args->mem_product = argv[2];
	args->mem_vendor = argv[3];

#define MAX_BANK_STATS_FILENAME_LEN 256
	args->bank_stats_file = calloc(MAX_BANK_STATS_FILENAME_LEN, 1);
	assert(args->bank_stats_file);

	/* -1 for the null byte, -2 for the two underscores */
	strncat(args->bank_stats_file, "./bank-stats/",
		MAX_BANK_STATS_FILENAME_LEN - 1 - 2);
	strncat(args->bank_stats_file, args->hostname,
		MAX_BANK_STATS_FILENAME_LEN - 1 - 2 -
			strlen(args->bank_stats_file));

	strcat(args->bank_stats_file, "_");

	strncat(args->bank_stats_file, args->mem_vendor,
		MAX_BANK_STATS_FILENAME_LEN - 1 - 2 -
			strlen(args->bank_stats_file));

	strcat(args->bank_stats_file, "_");

	strncat(args->bank_stats_file, args->mem_product,
		MAX_BANK_STATS_FILENAME_LEN - 1 - 2 -
			strlen(args->bank_stats_file));

	strncat(args->bank_stats_file, ".csv",
		MAX_BANK_STATS_FILENAME_LEN - 1 - 2 -
			strlen(args->bank_stats_file));

	printf("Hostname: %s. Mem. prod: %s. Mem. vendor: %s. Bank stat. file: %s\n",
	       args->hostname, args->mem_product, args->mem_vendor,
	       args->bank_stats_file);
}

static void stats_init_file_mapping(struct bank_stats *stats, int fd)
{
	stats->fd = fd;
	stats->offby = calibrate();
	stats->base = (unsigned long)mem_get_hp(ONE_GB);
	stats->pfn = virt_to_pfn(stats->base);

	{
		/* Quick check to see if it's a GB page */
		unsigned long tmp = stats->base;
		unsigned long prev = 0;

		for (size_t i = 0; i < 513; i++) {
			unsigned long pfn = virt_to_pfn(tmp);

			if (i) assert(pfn - prev == 1);

			tmp += 4096;
			prev = pfn;
		}
	}
}

static void import_bank_stats(struct bank_stats *stats)
{
	FILE *stream = fdopen(stats->fd, "r");
	char *lineptr = NULL;
	size_t n = 0;

	assert(stream);

	for (size_t l = 0; l < 3; l++) {
		size_t *import_to = NULL;

		if (l == 0) {
			import_to = &stats->bank_bit_ids[0];
		} else if (l == 1) {
			import_to = &stats->bank_functions[0];
		} else if (l == 2) {
			import_to = &stats->row_mask;
		}

		size_t m = getline(&lineptr, &n, stream);

		for (size_t i = 0, j = 0; i < m - 1; i++) {
			if (lineptr[i] == ',' || lineptr[i] == 0) {
				/* Two decimals and a comma */
				assert(i + 3 <= m);
				assert(j < DRAMA_MAX_NUM_BITS);

				if (l == 0) {
					int bank_bit = atoi(&lineptr[i + 1]);
					import_to[j++] = bank_bit;
					/* i += 3; */
				} else if (l == 1) {
					unsigned long bank_function = strtol(
						&lineptr[i + 1], NULL, 16);
					import_to[j++] = bank_function;
					/* i  */
				} else if (l == 2) {
					unsigned long row_mask = strtol(
						&lineptr[i + 1], NULL, 16);
					*import_to = row_mask;
				}
			}
		}

		free(lineptr);
	}

	printf(BANK_BITS_CSV);

	for (size_t i = 0; i < DRAMA_MAX_NUM_BITS; i++) {
		if (stats->bank_bit_ids[i]) {
			printf(",%lu", stats->bank_bit_ids[i]);
		}
	}

	printf("\n");
	printf(BANK_FUNCTIONS_CSV);

	for (size_t i = 0; i < DRAMA_MAX_NUM_FUNCTIONS; i++) {
		if (stats->bank_functions[i]) {
			printf(",0x%lx", stats->bank_functions[i]);
		}
	}

	printf("\n");
	printf(ROW_MASK_CSV);

	printf(",0x%lx\n", stats->row_mask);
}

void drama_import_bank_stats_from_file(const char *file, bool collect,
				       struct bank_stats *stats)
{
	int fd = open(file, O_RDONLY);

	stats_init_file_mapping(stats, fd);
	if (collect) collect_bank_stats(stats);
	import_bank_stats(stats);
}

static int main(int argc, char **argv)
{
	struct args args = { 0 };
	struct drama drama = { 0 };

	argparse(argc, argv, &args);

	srand(0);

	int fd = open(args.bank_stats_file, O_RDONLY);

	if (fd >= 0) {
		char tmp;
		if (!read(fd, &tmp, 1)) {
			close(fd);
			goto drama;
		}
	} else {
	drama:
		fd = open(args.bank_stats_file, O_RDWR | O_CREAT | O_APPEND,
			  S_IRUSR | S_IWUSR);

		assert(fd >= 0);

		stats_init_file_mapping(&drama.stats, fd);
		collect_bank_stats(&drama.stats);
		detect_banks(&drama);
		find_bank_bits(&drama);
		export_bank_bits(&drama);
		export_and_find_bank_functions(&drama);
		export_and_find_row_mask(&drama);
	}

	close(fd);
}
