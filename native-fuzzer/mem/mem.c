#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "mem.h"

/*
 * Huge pages and more. Relies on main.sh for mounting hugetlbfs!
 *
 * NOTE: don't forget the / at the end!
 */
#define MOUNTPOINT_TWO_MB "/mnt/huge/2m/"
#define MOUNTPOINT_ONE_GB "/mnt/huge/1g/"
#define MAX_MOUNTPOINT_LEN 32

/* FILENAME_LEN is 4: allows for ~450k filenames */
#define FILENAME_LEN 4

const char *mem_hp_mnts[] = { MOUNTPOINT_TWO_MB, MOUNTPOINT_ONE_GB };
const size_t mem_hp_sizes[] = { 1UL << 21, 1UL << 30 };

/*
 * Creates a new path at one of the specified mountpoints. Uses a global
 * counter to create a new file name
 */
static char *next_path(enum mem_hp_type type)
{
	const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";

	static unsigned long file_id = 0;

	size_t len = strnlen(mem_hp_mnts[type], MAX_MOUNTPOINT_LEN);
	char *path = calloc(len + FILENAME_LEN + 1, 1);

	assert(path);

	strcat(path, mem_hp_mnts[type]);
	assert(path[len - 1] == '/');

	unsigned long tmp = file_id;

	for (size_t i = 0; i < FILENAME_LEN; i++) {
		path[len + i] = alphabet[tmp % 26];
		tmp /= 26;
	}

	file_id++;

	return path;
}

unsigned long virt_to_pfn(unsigned long virt)
{
	unsigned long pte = 0;

	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd < 0) {
		perror(NULL);
	}

	assert(fd >= 0);
	assert(!(virt & 0xFFFUL));

	lseek(fd, 8 * (virt >> 12), SEEK_SET);
	assert(read(fd, &pte, 8) == 8);
	assert(pte & 0x8000000000000000UL); /* Present? */

	close(fd);

	return pte & 0x7FFFFFFFFFFFFFUL;
}

char *mem_get_hp(enum mem_hp_type type)
{
	int fd = open(next_path(type), O_CREAT | O_EXCL | O_RDWR,
		      S_IRUSR | S_IWUSR);

	assert(fd >= 0);

	char *addr = mmap(NULL, mem_hp_sizes[type], PROT_READ | PROT_WRITE,
			  MAP_PRIVATE, fd, 0);

	assert(addr);

	/* Populate! */
	memset(addr, 'A', mem_hp_sizes[type]);

	return addr;
}
