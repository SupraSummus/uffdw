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
	void * the_page = malloc(getpagesize());
	((char *)the_page)[0] = 1;

	struct uffdw_t * uffdw = uffdw_create();

	void * addr = mmap(
		NULL, page_size * 10,
		PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
		-1, 0
	);
	if (addr == (void *)-1) err(EXIT_FAILURE, "failed to map main range");
	if (!uffdw_register(uffdw, (size_t)addr, page_size * 10, handler, the_page)) abort();

	assert(((char *)addr)[1 * page_size] == 1);

	// get new addr in convenient area
	void * another_addr = mmap(
		NULL, page_size * 10,
		PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
		-1, 0
	);
	if (another_addr == (void *)-1) err(EXIT_FAILURE, "failed to map dummy range");
	if (munmap(another_addr, page_size * 10) != 0) err(EXIT_FAILURE, "failed to unmap dummy chunk");

	// remap
	void * addr_remaped = mremap(
		addr + page_size, page_size * 3, page_size * 3,
		MREMAP_MAYMOVE | MREMAP_FIXED, another_addr
	);
	if (addr_remaped != another_addr) err(EXIT_FAILURE, "failed to remap");
	assert(addr_remaped != addr); // just to be sure mem has moved

	assert(((char *)addr_remaped)[0 * page_size] == 1);
	assert(((char *)addr_remaped)[1 * page_size] == 2);
	assert(((char *)addr)[0 * page_size] == 3);
	assert(((char *)addr)[4 * page_size] == 4);

	uffdw_cancel(uffdw);

	return EXIT_SUCCESS;
}


