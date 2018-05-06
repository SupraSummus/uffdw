#include <assert.h>
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
	int uffd;
	pthread_t thread;
	pthread_mutex_t mutex;

	long pagesize;

	struct uffdw_range_t * ranges;
	struct uffdw_t * children;
	struct uffdw_t * next;
};

struct uffdw_range_t {
	/* function that handles pagefaults and its private data */
	uffdw_handler_t handler;
	void * handler_data;

	size_t offset;
	size_t end;

	/* access to addr `offset + i` is presented to handler as access to `handler_offset + i` */
	size_t handler_offset;

	struct uffdw_range_t * next;
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

static inline size_t _min(size_t a, size_t b) {
	if (a <= b) return a;
	return b;
}

static inline size_t _max(size_t a, size_t b) {
	if (a >= b) return a;
	return b;
}

static inline size_t _ranges_overlap(
	size_t off_a, size_t end_a,
	size_t off_b, size_t end_b,
	size_t * off, size_t * end
) {
	size_t _off = _max(off_a, off_b);
	size_t _end = _min(end_a, end_b);
	if (_off < _end) {
		if (off != NULL) *off = _off;
		if (end != NULL) *end = _end;
		return _end - _off;
	} else {
		return 0;
	}
}

static inline struct uffdw_t * _uffdw_alloc(void) {
	struct uffdw_t * uffdw = malloc(sizeof(struct uffdw_t));
	if (uffdw == NULL) return NULL;

	uffdw->uffd = -1;
	// TODO uffdw->thread
	uffdw->children = NULL;
	uffdw->ranges = NULL;
	if (pthread_mutex_init(&uffdw->mutex, NULL) != 0) {
		free(uffdw);
		return NULL;
	}

	return uffdw;
}

static void _uffdw_cleanup(struct uffdw_t * data) {
	if (data == NULL) return;

	// cancel all children
	struct uffdw_t * child = data->children;
	while (child != NULL) {
		struct uffdw_t * next = child->next;
		uffdw_cancel(child);
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

	// free ranges
	struct uffdw_range_t * range = data->ranges;
	while (range != NULL) {
		struct uffdw_range_t * next = range->next;
		free(range);
		range = next;
	}

	// free mutex
	if (pthread_mutex_destroy(&data->mutex) != 0) warnx("failed to destroy mutex");

	free(data);
}

static inline void _uffdw_attach_child(struct uffdw_t * parent, struct uffdw_t * child) {
	assert(child->next == NULL);
	child->next = parent->children;
	parent->children = child;
}

static inline struct uffdw_range_t * _uffdw_get_range(
	struct uffdw_t * uffdw, size_t offset, size_t end
) {
	for (
		struct uffdw_range_t * range = uffdw->ranges;
		range != NULL;
		range = range->next
	) {
		if ((_ranges_overlap(range->offset, range->end, offset, end, NULL, NULL)) > 0) {
			return range;
		}
	}
	return NULL;
}

static inline bool _uffdw_add_range(
	struct uffdw_t * uffdw,
	size_t offset, size_t end, size_t handler_offset,
	uffdw_handler_t handler, void * private_data
) {
	assert(offset <= end);
	if (end == offset) return true;
	assert(_uffdw_get_range(uffdw, offset, end) == NULL);

	struct uffdw_range_t * range = malloc(sizeof(struct uffdw_range_t));
	if (range == NULL) return false;

	range->offset = offset;
	range->end = end;
	range->handler_offset = handler_offset;
	range->handler = handler;
	range->handler_data = private_data;
	range->next = uffdw->ranges;
	uffdw->ranges = range;

	return true;
}

static inline void _uffdw_remove_range(
	struct uffdw_t * uffdw, size_t offset, size_t end
) {
	struct uffdw_range_t * * range_p = &(uffdw->ranges);
	while (*range_p != NULL) {
		struct uffdw_range_t * range  = *range_p;
		size_t o, e;
		size_t s = _ranges_overlap(range->offset, range->end, offset, end, &o, &e);
		if (s > 0) {
			// unlink previous range
			*range_p = range->next;
			range->next = NULL;

			// add split ranges
			if (
				!_uffdw_add_range(
					uffdw,
					range->offset, o, range->handler_offset,
					range->handler, range->handler_data
				) ||
				!_uffdw_add_range(
					uffdw,
					e, range->end, range->handler_offset - range->offset + e,
					range->handler, range->handler_data
				)
			) warnx("failed to add split ranges");

			free(range);
		} else {
			range_p = &(range->next);
		}
	}
}

static void * _uffdw_run(void * _uffdw) {
	struct uffdw_t * uffdw = _uffdw;

	while (true) {
		struct uffd_msg msg;
		if (read(uffdw->uffd, &msg, sizeof(msg)) != sizeof(msg)) {
			if (DEBUG) perror("failed to read uffd message");
			return NULL;
		}

		bool flag_write;
		struct uffdw_t * new_uffdw;

		if (pthread_mutex_lock(&uffdw->mutex) != 0) {
			warnx("failed to acquire lock");
			return NULL;
		}

		switch (msg.event) {
			case UFFD_EVENT_PAGEFAULT: {
				flag_write = (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) != 0;
				LOG("uffd %d: got PAGEFAULT (%p, FLAG_WRITE=%d)", uffdw->uffd, (void *)msg.arg.pagefault.address, flag_write);
				if (flag_write) {
					LOG("error: write faults are not supported");
					pthread_mutex_unlock(&uffdw->mutex);
					return NULL;
				}

				struct uffdw_range_t * range = _uffdw_get_range(
					uffdw,
					msg.arg.pagefault.address, msg.arg.pagefault.address + 1
				);
				if (range == NULL) {
					warnx("uffd %d: PAGEFAULT on non registered page %p", uffdw->uffd, (void *)msg.arg.pagefault.address);
					uffdw_zeropage(uffdw->uffd, msg.arg.pagefault.address, uffdw->pagesize);
				} else {
					if(!range->handler(
						uffdw->uffd,
						msg.arg.pagefault.address - range->offset + range->handler_offset,
						msg.arg.pagefault.address,
						range->handler_data
					)) {
						LOG("error: uffdw handler failed");
						pthread_mutex_unlock(&uffdw->mutex);
						return NULL;
					}
				}

				break;
			}

			case UFFD_EVENT_FORK: {
				LOG("uffd %d: got FORK (new uffd %d)", uffdw->uffd, msg.arg.fork.ufd);

				// copy structure
				new_uffdw = _uffdw_alloc();
				new_uffdw->uffd = msg.arg.fork.ufd;
				struct uffdw_range_t * range = uffdw->ranges;
				while (range != NULL) {
					if (!_uffdw_add_range(
						new_uffdw,
						range->offset, range->end, range->handler_offset,
						range->handler, range->handler_data
					)) {
						warn("failed to store range data");
						_uffdw_cleanup(new_uffdw);
						pthread_mutex_unlock(&uffdw->mutex);
						return NULL;
					}
					range = range->next;
				}

				// attach to children list
				_uffdw_attach_child(uffdw, new_uffdw);

				// run thread
				if (pthread_create(&(new_uffdw->thread), NULL, _uffdw_run, new_uffdw) != 0) {
					if (DEBUG) perror("failed to create child uffdw thread");
					_uffdw_cleanup(new_uffdw);
					pthread_mutex_unlock(&uffdw->mutex);
					return NULL;
				}

				break;
			}

			case UFFD_EVENT_REMAP: {
				LOG(
					"uffd %d: got REMAP (%zu, %p -> %p)", uffdw->uffd,
					(size_t)msg.arg.remap.len, (void *)msg.arg.remap.from, (void *)msg.arg.remap.to
				);

				struct uffdw_range_t * range = _uffdw_get_range(
					uffdw,
					msg.arg.remap.from, msg.arg.remap.from + 1
				);
				if (range == NULL) {
					warnx("uffd %d: REMAP on non registered addr %p", uffdw->uffd, (void *)msg.arg.remap.from);
				} else {
					if (!_uffdw_add_range(
						uffdw,
						msg.arg.remap.to, msg.arg.remap.to + msg.arg.remap.len, msg.arg.remap.from,
						range->handler, range->handler_data
					)) warnx("uffd %d: failed to store range data", uffdw->uffd);
				}

				break;
			}

			case UFFD_EVENT_REMOVE: {
				LOG("uffd %d: got REMOVE (%p - %p)", uffdw->uffd, (void *)msg.arg.remove.start, (void *)msg.arg.remove.end);
				warnx("UFFD_EVENT_REMOVE handling not implemented yet");
				break;
			}

			case UFFD_EVENT_UNMAP: {
				LOG("uffd %d: got UNMAP (%p - %p)", uffdw->uffd, (void *)msg.arg.remove.start, (void *)msg.arg.remove.end);
				_uffdw_remove_range(
					uffdw,
					msg.arg.remove.start, msg.arg.remove.end
				);
				break;
			}

			default: {
				LOG("uffd %d: error: got unsupported message type", uffdw->uffd);
				pthread_mutex_unlock(&uffdw->mutex);
				return NULL;
			}
		}

		pthread_mutex_unlock(&uffdw->mutex);

	}
}

struct uffdw_t * uffdw_create() {
	struct uffdw_t * data = _uffdw_alloc();
	if (data == NULL) {
		warn("failed to allocate uffdw struture");
		return NULL;
	}

	data->pagesize = sysconf(_SC_PAGESIZE);
	if (data->pagesize < 0) {
		warn("failed to get pagesize");
		_uffdw_cleanup(data);
		return NULL;
	}

	data->uffd = syscall(SYS_userfaultfd, O_CLOEXEC);
	if (data->uffd < 0) {
		warn("failed to open userfaultfd descriptor");
		_uffdw_cleanup(data);
		return NULL;
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
		warn("failed to handshake with userfaultfd API");
		_uffdw_cleanup(data);
		return NULL;
	}
	if (!(api_options.ioctls & (1 << _UFFDIO_REGISTER))) {
		warnx("got invalid response to uffd handshake");
		_uffdw_cleanup(data);
		return NULL;
	}

	if(pthread_create(&(data->thread), NULL, _uffdw_run, data)) {
		warnx("failed to create uffdw thread");
		_uffdw_cleanup(data);
		return NULL;
	}

	return data;
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

int _uffdw_get_uffd(struct uffdw_t * data) {
	return data->uffd;
}

bool uffdw_register(
	struct uffdw_t * uffdw,
	size_t offset, size_t size, size_t handler_offset,
	uffdw_handler_t handler, void * private_data
) {
	LOG("uffd %d: register %p - %p", uffdw->uffd, (void *)offset, (void *)(offset + size));

	if (pthread_mutex_lock(&uffdw->mutex) != 0) {
		warnx("failed to acquire lock");
		return false;
	}

	// alloc and attach range structure
	if (!_uffdw_add_range(
		uffdw,
		offset, offset + size, handler_offset,
		handler, private_data
	)) {
		warn("failed to store uffdw range data");
		pthread_mutex_unlock(&uffdw->mutex);
		return false;
	}

	// prepare data for syscall
	struct uffdio_register reg;
	reg.range.start = offset;
	reg.range.len = size;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	reg.ioctls = 0;

	// do syscall
	if (ioctl(uffdw->uffd, UFFDIO_REGISTER, &reg) != 0) {
		warn("uffd register syscall failed");
		_uffdw_remove_range(uffdw, offset, offset + size);
		pthread_mutex_unlock(&uffdw->mutex);
		return false;
	}

	pthread_mutex_unlock(&uffdw->mutex);
	return true;
}

bool uffdw_copy(int uffd, void * our_offset, size_t target_offset, size_t size) {
	struct uffdio_copy copy;
	copy.dst = target_offset;
	copy.src = (size_t)our_offset;
	copy.len = size;
	copy.mode = 0;
	copy.copy = 0;

	bool ret = ioctl(uffd, UFFDIO_COPY, &copy) == 0;
	if (DEBUG && !ret) warn("copy failed");
	return ret;
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

bool uffdw_zeropage(int uffd, size_t offset, size_t size) {
	struct uffdio_zeropage uffdio;
	uffdio.range.start = offset;
	uffdio.range.len = size;
	uffdio.mode = 0;
	uffdio.zeropage = 0;

	if (ioctl(uffd, UFFDIO_ZEROPAGE, &uffdio) != 0) {
		if (DEBUG) warn("copy failed");
		return false;
	}
	if (uffdio.zeropage < 0) {
		return false;
	}
	return (size_t)uffdio.zeropage == size;
}

bool uffdw_wake(int uffd, size_t offset, size_t size) {
	struct uffdio_range range;
	range.start = offset;
	range.len = size;

	bool ret = ioctl(uffd, UFFDIO_WAKE, &range) == 0;
	if (DEBUG && !ret) warn("wake failed");
	return ret;
}
