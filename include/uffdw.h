#ifndef UFFDW_H
#define UFFDW_H

#include <stdbool.h>

/**
 * Function to handle pagefaults. It should do its things (propably
 * call `uffdw_copy()`, `uffdw_zeropage()`, ...) and return boolean if
 * it was successful.
 *
 * As arguments it takes two kinds of pagefault offsets (`page_offset`
 * and `real_page_offset`). The first one is page address calculated
 * taking remaps into account. Use it to figure out which pages should
 * go to faulting area. The second one is original page address. Use it
 * as argument to `uffdw_copy()` and similiar functions.
 */
typedef bool (* uffdw_handler_t) (
	int uffd,
	size_t page_offset, size_t real_page_offset,
	void * private_data
);

struct uffdw_t;

struct uffdw_t * uffdw_create();
void uffdw_cancel(struct uffdw_t * data);

int _uffdw_get_uffd(struct uffdw_t *);

bool uffdw_register(struct uffdw_t * uffdw, size_t offset, size_t size, uffdw_handler_t handler, void * private_data);
//bool uffdw_unregister(int uffd, size_t offset, size_t size);

bool uffdw_copy(int uffd, void * our_offset, size_t target_offset, size_t size);
bool uffdw_copy_from_fd(int uffd, int fd, size_t offset, size_t size);
bool uffdw_zeropage(int uffd, size_t offset, size_t size);
bool uffdw_wake(int uffd, size_t offset, size_t size);

#endif
