#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <uffdw.h>
#include <unistd.h>

bool handler(int uffd, size_t page, size_t page_original, void * the_page) {
	(void)page;
	bool r = uffdw_copy(uffd, the_page, page_original, sysconf(_SC_PAGESIZE));
	((char *)the_page)[0] ++;
	return r;
}

int main() {
	int page_size = sysconf(_SC_PAGESIZE);

	// create handling thread
	void * the_page = malloc(page_size);
	*((char *)the_page) = (char)0;

	struct uffdw_t * uffdw = uffdw_create();
	if (uffdw == NULL) abort();

	// get pages in conveniet area and reagister them to be handled by our thread
	void * addr1 = mmap(
		NULL, getpagesize() * 10,
		PROT_READ, MAP_SHARED | MAP_ANONYMOUS,
		-1, 0
	);
	void * addr2 = mmap(
		NULL, getpagesize() * 10,
		PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
		-1, 0
	);

	if (!uffdw_register(uffdw, (size_t)addr1, sysconf(_SC_PAGESIZE) * 10, handler, the_page)) abort();
	if (!uffdw_register(uffdw, (size_t)addr2, sysconf(_SC_PAGESIZE) * 10, handler, the_page)) abort();

	// do some checks in child and parent
	assert(((char *)addr1)[page_size * 0] == 0);
	assert(((char *)addr2)[page_size * 0] == 1);
	int pid = fork();
	if (pid == 0) {
		assert(((char *)addr1)[page_size * 0] == 0);
		assert(((char *)addr1)[page_size * 1] == 2);
		assert(((char *)addr2)[page_size * 0] == 1);
		assert(((char *)addr2)[page_size * 1] == 3);
		return EXIT_SUCCESS;

	} else {
		int s;
		if (waitpid(pid, &s, 0) != pid) abort();
		assert(((char *)addr1)[page_size * 1] == 2);
		assert(((char *)addr1)[page_size * 2] == 4);
		assert(((char *)addr2)[page_size * 1] == 5);
		assert(((char *)addr2)[page_size * 2] == 6);
		uffdw_cancel(uffdw);
		return s;
	}
}
