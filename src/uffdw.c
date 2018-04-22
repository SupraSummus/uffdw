#include <err.h>
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
#ifdef NDEBUG
	#define DEBUG false
#endif

#if DEBUG
	#define LOG(format, ...) do { fprintf(stderr, format "\n", ##__VA_ARGS__); } while (0)
#else
	#define LOG(...)
#endif

struct uffdw_t {
	uffdw_handler_t handler;
	void * handler_data;
	int uffd;
	pthread_t thread;
	struct uffdw_list_t * children;
};

struct uffdw_list_t {
	struct uffdw_t * uffdw;
	struct uffdw_list_t * next;
};

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

static void _uffdw_cleanup(struct uffdw_t * data) {
	if (data == NULL) return;

	// cancel all children
	struct uffdw_list_t * child = data->children;
	while (child != NULL) {
		uffdw_cancel(child->uffdw);
		struct uffdw_list_t * next = child->next;
		free(child);
		child = next;
	}
	data->children = NULL;

	// close uffd
	if (data->uffd >= 0) {
		if (close(data->uffd) != 0) {
			warn("there was a problem during closing userfaultfd descriptor");
		}
	}
	data->uffd = -1;

	free(data);
}

static void * _uffdw_run(void * _data) {
	struct uffdw_t * data = _data;

	while (true) {
		struct uffd_msg msg;
		if (read(data->uffd, &msg, sizeof(msg)) != sizeof(msg)) {
			if (DEBUG) perror("failed to read uffd message");
			return NULL;
		}

		bool flag_write;
		struct uffdw_t * new_data;

		switch (msg.event) {
			case UFFD_EVENT_PAGEFAULT: {
				flag_write = (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) != 0;
				LOG("uffd %d: got PAGEFAULT (%p, FLAG_WRITE=%d)", data->uffd, (void *)msg.arg.pagefault.address, flag_write);
				if (flag_write) {
					LOG("error: write faults are not supported");
					return NULL;
				}

				if(!data->handler(data->uffd, msg.arg.pagefault.address, data->handler_data)) {
					LOG("error: uffdw handler failed");
					return NULL;
				}

				break;
			}

			case UFFD_EVENT_FORK: {
				LOG("uffd %d: got FORK (new uffd %d)", data->uffd, msg.arg.fork.ufd);

				new_data = malloc(sizeof(struct uffdw_t));
				new_data->handler = data->handler;
				new_data->handler_data = data->handler_data;
				new_data->uffd = msg.arg.fork.ufd;
				new_data->children = NULL;

				if (pthread_create(&(new_data->thread), NULL, _uffdw_run, new_data) != 0) {
					if (DEBUG) perror("failed to create child uffdw thread");
					_uffdw_cleanup(new_data);
					return NULL;
				}

				// attach to children list
				struct uffdw_list_t * child = malloc(sizeof(struct uffdw_list_t));
				child->uffdw = new_data;
				child->next = data->children;
				data->children = child;

				break;
			}

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

struct uffdw_t * uffdw_create(uffdw_handler_t handler, void * private_data) {
	struct uffdw_t * data = malloc(sizeof(struct uffdw_t));
	data->handler = handler;
	data->handler_data = private_data;
	data->uffd = -1;
	data->children = NULL;

	data->uffd = syscall(SYS_userfaultfd, O_CLOEXEC);
	if (data->uffd < 0) {
		if (DEBUG) perror("failed to open userfaultfd descriptor");
		goto cleanup;
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
		goto cleanup;
	}
	if (!(api_options.ioctls & (1 << _UFFDIO_REGISTER))) {
		LOG("got invalid response to uffd handshake");
		goto cleanup;
	}

	if(pthread_create(&(data->thread), NULL, _uffdw_run, data)) {
		if (DEBUG) perror("failed to create uffdw thread");
		goto cleanup;
	}

	return data;

	cleanup:
	_uffdw_cleanup(data);
	return NULL;
}

void uffdw_cancel(struct uffdw_t * data) {
	LOG("uffd %d: canceling", data->uffd);

	// cancel and wait for thread
	if (pthread_cancel(data->thread) != 0) {
		warn("failed to cancel uffdw thread");
	}
	if (pthread_join(data->thread, NULL) != 0) {
		warn("there was a problem during joining uffdw thread");
	}

	// clean thread structure
	_uffdw_cleanup(data);
}

int uffdw_get_uffd(struct uffdw_t * data) {
	return data->uffd;
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
