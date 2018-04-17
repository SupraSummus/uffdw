#include <assert.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stropts.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <uffdw.h>

#ifndef DEBUG
	#define DEBUG false
#endif

#if DEBUG
	#define LOG(format, ...) do { fprintf(stderr, format "\n", ##__VA_ARGS__); } while (0)
#else
	#define LOG(...)
#endif

static inline size_t _read_exact(int fd, void * buf, size_t size) {
	off_t offset = 0;
	while (size > 0) {
		ssize_t read_result = read(fd, buf, size);
		if (read_result <= 0) return offset;
		size -= read_result;
		offset += read_result;
	}
	return offset;
}

static void * _uffdw_run(void * _data) {
	struct uffdw_data_t * data = _data;

	while(true) {
		struct uffd_msg msg;
		if (read(data->uffd, &msg, sizeof(msg)) != sizeof(msg)) {
			if (DEBUG) perror("failed to uffd read message");
			return NULL;
		}

		bool flag_write;

		switch(msg.event) {
			case UFFD_EVENT_PAGEFAULT:
				flag_write = (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) != 0;
				LOG("got PAGEFAULT (%p, FLAG_WRITE=%d)", (void *)msg.arg.pagefault.address, flag_write);
				if (flag_write) {
					LOG("error: write faults are not supported");
					return NULL;
				}

				if(!data->handler(data->uffd, msg.arg.pagefault.address, data->handler_data)) {
					LOG("error: uffdw handler failed");
					return NULL;
				};

				break;

			case UFFD_EVENT_FORK:
				LOG("got FORK");
				goto unsupported_message_type;
			case UFFD_EVENT_REMAP:
				LOG("got REMAP");
				goto unsupported_message_type;
			case UFFD_EVENT_REMOVE:
				LOG("got REMOVE");
				goto unsupported_message_type;
			case UFFD_EVENT_UNMAP:
				LOG("got UNMAP");
				goto unsupported_message_type;
			default:
				unsupported_message_type:
				LOG("error: got unsupported message type");
				return NULL;
		}

	}
}

bool uffdw_create(struct uffdw_data_t * data) {
	data->uffd = syscall(SYS_userfaultfd, O_CLOEXEC);
	if (data->uffd < 0) {
		if (DEBUG) perror("failed to open userfaultfd descriptor");
		return false;
	}

	struct uffdio_api api_options;
	api_options.api = UFFD_API;
	api_options.features = (
		UFFD_FEATURE_EVENT_FORK |
		UFFD_FEATURE_EVENT_REMAP |
		UFFD_FEATURE_EVENT_REMOVE |
		UFFD_FEATURE_EVENT_UNMAP |
		UFFD_FEATURE_MISSING_HUGETLBFS |
		UFFD_FEATURE_MISSING_SHMEM
	);
	api_options.ioctls = 0;

	if (ioctl(data->uffd, UFFDIO_API, &api_options) != 0) {
		if (DEBUG) perror("failed to handshake with userfaultfd API");
		close(data->uffd);
		return false;
	}
	assert(api_options.ioctls & (1 << _UFFDIO_REGISTER));

	if(pthread_create(&(data->thread), NULL, _uffdw_run, data)) {
		if (DEBUG) perror("failed to create uffdw thread");
		close(data->uffd);
		return false;
	}

	return true;
}

void uffdw_destroy(struct uffdw_data_t * data) {
	if (close(data->uffd) != 0) {
		if (DEBUG) perror("there was a problem during closing userfaultfd descriptor");
	}
	data->uffd = -1;

	if (pthread_cancel(data->thread) != 0) {
		LOG("error: failed to cancel uffdw thread");
	}
	void * retval = NULL;
	if (pthread_join(data->thread, &retval) != 0) {
		LOG("error: there was a problem during joining uffdw thread");
	}
}

bool uffdw_register(int uffd, size_t offset, size_t size) {
	LOG("register %zu bytes at offset %p", size, (void *)offset);
	struct uffdio_register reg;
	reg.range.start = offset;
	reg.range.len = size;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	reg.ioctls = 0;

	return ioctl(uffd, UFFDIO_REGISTER, &reg) == 0;
}

bool uffdw_copy(int uffd, void * our_offset, size_t target_offset, size_t size) {
	struct uffdio_copy copy;
	copy.dst = target_offset;
	copy.src = (size_t)our_offset;
	copy.len = size;
	copy.mode = 0;
	copy.copy = 0;

	return ioctl(uffd, UFFDIO_COPY, &copy) == 0;
}

bool uffdw_copy_from_fd(int uffd, int fd, size_t offset, size_t size) {
	void * d = malloc(size);
	if (d == NULL) return false;
	if (_read_exact(fd, d, size) != size) {
		free(d);
		return false;
	}
	if (!uffdw_copy(uffd, d, offset, size)) {
		free(d);
		return false;
	}
	free(d);
	return true;
}
