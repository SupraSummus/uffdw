#ifndef UFFDW_H
#define UFFDW_H

#include <pthread.h>
#include <stdbool.h>

typedef bool (* uffdw_handler_t) (int uffd, size_t page_offset, void * private_data);

struct uffdw_data_t {
	uffdw_handler_t handler;
	void * handler_data;
	int uffd;
	pthread_t thread;
};

bool uffdw_create(struct uffdw_data_t * data);
void uffdw_destroy(struct uffdw_data_t * data);

bool uffdw_register(int uffd, size_t offset, size_t size);

bool uffdw_copy(int uffd, void * our_offset, size_t target_offset, size_t size);
bool uffdw_copy_from_fd(int uffd, int fd, size_t offset, size_t size);

#endif
