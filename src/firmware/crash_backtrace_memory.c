/* Memory for libbacktrace, in place of its alloc.c, mmap.c, read.c and mmapio.c.
 *
 * libbacktrace cannot free a state, and a state reads the list of loaded objects only once. So
 * the crash symbolizer makes a new state after images were loaded, and each one kept tens of
 * MiB of debug information until the process exited: on a 32-bit host, a few hundred crash
 * reports used up the address space. Here, every allocation is on one list, and
 * qsim_backtrace_free_all frees all of them before the next state is made. File views are read
 * into the same memory, so nothing stays mapped either.
 *
 * Not thread-safe: the symbolizer holds a lock around every libbacktrace call. */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "backtrace.h"
#include "internal.h"

void qsim_backtrace_free_all(void);

typedef union block {
	struct {
		union block* prev;
		union block* next;
	} links;

	max_align_t alignment;
} block;

static block* blocks;

static void link_block(block* b) {
	b->links.prev = NULL;
	b->links.next = blocks;
	if (blocks) blocks->links.prev = b;
	blocks = b;
}

static void unlink_block(block* b) {
	if (b->links.prev) b->links.prev->links.next = b->links.next;
	else blocks = b->links.next;
	if (b->links.next) b->links.next->links.prev = b->links.prev;
}

static void* resize(void* memory, size_t size) {
	if (size > SIZE_MAX - sizeof(block)) {
		errno = ENOMEM;
		return NULL;
	}
	block* old = memory ? (block*)memory - 1 : NULL;
	if (old) unlink_block(old);
	block* b = realloc(old, sizeof(block) + size);
	if (!b) {
		if (old) link_block(old);
		return NULL;
	}
	link_block(b);
	return b + 1;
}

static void release(void* memory) {
	if (!memory) return;
	block* b = (block*)memory - 1;
	unlink_block(b);
	free(b);
}

void qsim_backtrace_free_all(void) {
	while (blocks) {
		block* next = blocks->links.next;
		free(blocks);
		blocks = next;
	}
}

void* backtrace_alloc(struct backtrace_state* state, size_t size, backtrace_error_callback error_callback, void* data) {
	(void)state;
	void* memory = resize(NULL, size);
	if (!memory && error_callback) error_callback(data, "malloc", errno);
	return memory;
}

void backtrace_free(struct backtrace_state* state, void* memory, size_t size, backtrace_error_callback error_callback,
					void* data) {
	(void)state;
	(void)size;
	(void)error_callback;
	(void)data;
	release(memory);
}

/* The same growth as libbacktrace's alloc.c. */
void* backtrace_vector_grow(struct backtrace_state* state, size_t size, backtrace_error_callback error_callback,
							void* data, struct backtrace_vector* vec) {
	(void)state;
	if (size > vec->alc) {
		size_t alc;
		if (vec->size == 0) alc = 32 * size;
		else if (vec->size >= 4096) alc = vec->size + 4096;
		else alc = 2 * vec->size;
		if (alc < vec->size + size) alc = vec->size + size;
		void* base = resize(vec->base, alc);
		if (!base) {
			error_callback(data, "realloc", errno);
			return NULL;
		}
		vec->base = base;
		vec->alc  = alc - vec->size;
	}
	void* added = (char*)vec->base + vec->size;
	vec->size += size;
	vec->alc -= size;
	return added;
}

int backtrace_vector_release(struct backtrace_state* state, struct backtrace_vector* vec,
							 backtrace_error_callback error_callback, void* data) {
	(void)state;
	vec->alc = 0;
	if (vec->size == 0) {
		release(vec->base);
		vec->base = NULL;
		return 1;
	}
	void* base = resize(vec->base, vec->size);
	if (!base) {
		error_callback(data, "realloc", errno);
		return 0;
	}
	vec->base = base;
	return 1;
}

void* backtrace_vector_finish(struct backtrace_state* state, struct backtrace_vector* vec,
							  backtrace_error_callback error_callback, void* data) {
	if (!backtrace_vector_release(state, vec, error_callback, data)) return NULL;
	void* finished = vec->base;
	vec->base      = NULL;
	vec->size      = 0;
	vec->alc       = 0;
	return finished;
}

int backtrace_get_view(struct backtrace_state* state, int descriptor, off_t offset, uint64_t size,
					   backtrace_error_callback error_callback, void* data, struct backtrace_view* view) {
	if ((uint64_t)(size_t)size != size) {
		error_callback(data, "file size too large", 0);
		return 0;
	}
	if (lseek(descriptor, offset, SEEK_SET) < 0) {
		error_callback(data, "lseek", errno);
		return 0;
	}
	char* base = backtrace_alloc(state, (size_t)size, error_callback, data);
	if (!base) return 0;
	size_t got = 0;
	while (got < size) {
		/* Windows reads at most INT_MAX bytes at once. */
		const size_t  chunk      = size - got < (1u << 30) ? (size_t)(size - got) : (1u << 30);
		const ssize_t read_bytes = read(descriptor, base + got, chunk);
		if (read_bytes < 0) {
			error_callback(data, "read", errno);
			release(base);
			return 0;
		}
		if (read_bytes == 0) {
			error_callback(data, "file too short", 0);
			release(base);
			return 0;
		}
		got += (size_t)read_bytes;
	}
	view->base = base;
	view->data = base;
	view->len  = (size_t)size;
	return 1;
}

void backtrace_release_view(struct backtrace_state* state, struct backtrace_view* view,
							backtrace_error_callback error_callback, void* data) {
	backtrace_free(state, view->base, view->len, error_callback, data);
	view->data = NULL;
	view->base = NULL;
}
