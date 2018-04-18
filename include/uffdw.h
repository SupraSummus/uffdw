#ifndef UFFDW_H
#define UFFDW_H

#include <stdbool.h>

typedef bool (* uffdw_handler_t) (int uffd, size_t page_offset, void * private_data);

struct uffdw_data_t;

struct uffdw_data_t * uffdw_create(uffdw_handler_t handler, void * private_data);
void uffdw_cancel(struct uffdw_data_t * data);

int uffdw_get_uffd(struct uffdw_data_t *);

bool uffdw_register(int uffd, size_t offset, size_t size);

bool uffdw_copy(int uffd, void * our_offset, size_t target_offset, size_t size);
bool uffdw_copy_from_fd(int uffd, int fd, size_t offset, size_t size);

#endif
