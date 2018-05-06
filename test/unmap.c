#define _GNU_SOURCE

#include <assert.h>
#include <err.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <uffdw.h>
#include <unistd.h>

bool handler(int uffd, size_t page, size_t page_original, void * the_page) {
	(void)page;
	bool result = uffdw_copy(uffd, the_page, page_original, sysconf(_SC_PAGESIZE));
	((char *)the_page)[0] ++;
	return result;
}

int main () {
	int page_size = sysconf(_SC_PAGESIZE);
	void * the_page1 = malloc(getpagesize());
	((char *)the_page1)[0] = 1;
	void * the_page2 = malloc(getpagesize());
	((char *)the_page2)[0] = 42;

	struct uffdw_t * uffdw = uffdw_create();

	// map and register 10 pages
	void * addr = mmap(
		NULL, page_size * 10,
		PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
		-1, 0
	);
	if (addr == (void *)-1) err(EXIT_FAILURE, "failed to map main range");
	if (!uffdw_register(
		uffdw,
		(size_t)addr, page_size * 10, (size_t)addr,
		handler, the_page1
	)) abort();

	assert(((char *)addr)[3 * page_size] == 1);
	assert(((char *)addr)[4 * page_size] == 2);

	// unmap pages 2 and 3 and map zeroes
	if (munmap(addr + 2 * page_size, 2 * page_size)) err(EXIT_FAILURE, "failed to unmap");
	if (mmap(
		addr + 2 * page_size, 2 * page_size,
		PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		-1, 0
	) != addr + 2 * page_size) err(EXIT_FAILURE, "failed to map little range");

	// on page 3 there should be zero (mapped /dev/zero)
	assert(((char *)addr)[3 * page_size] == 0);

	// register new area for handling
	if (!uffdw_register(
		uffdw,
		(size_t)addr + 2 * page_size, page_size * 2, (size_t)addr + 2 * page_size,
		handler, the_page2
	)) abort();

	// pages already in place
	assert(((char *)addr)[3 * page_size] == 0);
	assert(((char *)addr)[4 * page_size] == 2);
	// page handled be second handler
	assert(((char *)addr)[2 * page_size] == 42);
	// page handled by first handler
	assert(((char *)addr)[1 * page_size] == 3);

	uffdw_cancel(uffdw);

	return EXIT_SUCCESS;
}


