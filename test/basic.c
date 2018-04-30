#include <assert.h>
#include <err.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <uffdw.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

bool handler(int uffd, size_t page, size_t page_original, void * the_page) {
	(void)page;
	bool result = uffdw_copy(uffd, the_page, page_original, sysconf(_SC_PAGESIZE));
	((char *)the_page)[42] ++;
	return result;
}

int main () {
	void * the_page = malloc(getpagesize());
	memcpy(the_page, "0123456789", 10);

	struct uffdw_t * uffdw = uffdw_create();
	if (uffdw == NULL) err(EXIT_FAILURE, "failed to create uffdw");

	// mmap anonymous
	void * addr = mmap(
		NULL, getpagesize() * 10,
		PROT_READ, MAP_SHARED | MAP_ANONYMOUS,
		-1, 0
	);
	if (!uffdw_register(uffdw, (size_t)addr, sysconf(_SC_PAGESIZE) * 10, handler, the_page)) abort();

	for (int i = 0; i < 10; i ++) {
		assert(((char *)addr)[i * sysconf(_SC_PAGESIZE) + i] == '0' + i);
		assert(((char *)addr)[i * sysconf(_SC_PAGESIZE) + 42] == (char)i);
	}

	uffdw_cancel(uffdw);

	return EXIT_SUCCESS;
}


