#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <uffdw.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

bool handler(int uffd, size_t page, void * the_page) {
	bool result = uffdw_copy(uffd, the_page, page, sysconf(_SC_PAGESIZE));
	((char *)the_page)[42] ++;
	return result;
}

int main () {
	void * the_page = malloc(getpagesize());
	memcpy(the_page, "0123456789", 10);

	struct uffdw_data_t * uffdw = uffdw_create(handler, the_page);

	// mmap anonymous
	void * addr = mmap(
		NULL, getpagesize() * 10,
		PROT_READ, MAP_SHARED | MAP_ANONYMOUS,
		-1, 0
	);
	assert(uffdw_register(uffdw_get_uffd(uffdw), (size_t)addr, sysconf(_SC_PAGESIZE) * 10));

	for (int i = 0; i < 10; i ++) {
		assert(((char *)addr)[i * sysconf(_SC_PAGESIZE) + i] == '0' + i);
		assert(((char *)addr)[i * sysconf(_SC_PAGESIZE) + 42] == (char)i);
	}

	uffdw_cancel(uffdw);

	return EXIT_SUCCESS;
}


