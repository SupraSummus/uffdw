#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <uffdw.h>
#include <unistd.h>

bool handler(int uffd, size_t page, void * the_page) {
	bool r = uffdw_copy(uffd, the_page, page, sysconf(_SC_PAGESIZE));
	((char *)the_page)[0] ++;
	return r;
}

int main() {
	int page_size = sysconf(_SC_PAGESIZE);

	// create handling thread
	void * the_page = malloc(page_size);
	*((char *)the_page) = (char)0;

	struct uffdw_data_t * uffdw = uffdw_create(handler, the_page);
	if (uffdw == NULL) abort();

	// get 10 pages in conveniet area and reagister them to be handled by our thread
	void * addr = mmap(
		NULL, getpagesize() * 10,
		PROT_READ, MAP_SHARED | MAP_ANONYMOUS,
		-1, 0
	);

	if (!uffdw_register(uffdw_get_uffd(uffdw), (size_t)addr, sysconf(_SC_PAGESIZE) * 10)) abort();

	// do some checks in child and parent
	assert(((char *)addr)[page_size * 6] == 0);
	int pid = fork();
	if (pid == 0) {
		assert(((char *)addr)[page_size * 6] == 0);
		assert(((char *)addr)[page_size * 3] == 1);
		assert(((char *)addr)[page_size * 4] == 2);
		return EXIT_SUCCESS;

	} else {
		int s;
		if (waitpid(pid, &s, 0) != pid) abort();
		assert(((char *)addr)[page_size * 3] == 1);
		assert(((char *)addr)[page_size * 5] == 3);
		uffdw_cancel(uffdw);
		return s;
	}
}
