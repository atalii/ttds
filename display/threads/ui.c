#include "../threads/ui.h"

#include "../abort.h"
#include "../rendering/rendering.h"
#include "rendering/canvas.h"
#include "termination.h"

#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PANES 1024
#define PANE_DELAY 1000

struct pane {
	char *name;
	struct canvas *canvas;
};

struct pane_storage {
	size_t count;
	pthread_mutex_t lock;
	struct pane panes[MAX_PANES];
};

struct ui_ctx {
	struct rendering_vtable vt;
	void *r_ctx; // type erased rendering context

	struct pane_storage panes;

	int cancellation_fd;

	int sync_fd_rx;
	int sync_fd_tx;
};

static void *rotate_panes(void *);

/* Search for a pane with the given name, or null if none is found.
 * This does not perform any synchronization, so if there are other threads
 * accessing the panes, you must lock the mutex first. */
static struct pane *lookup_pane_thread_unsafe(
    struct pane_storage *, const char *);

char *ui_failure_strs[] = {
	[UI_OK] = "no failure",
	[UI_DUPLICATE] = "duplicate pane",
	[UI_OOM] = "oom",
	[UI_NO_SUCH_PANE] = "targeted pane doesn't exist",
	[UI_TOO_MANY_PANES] = "MAX_PANES would be exceeded",
};

char *ui_failure_str(enum ui_failure f)
{
	return ui_failure_strs[f];
}

struct ui_ctx *ui_ctx_new(struct rendering_vtable vt)
{
	struct ui_ctx *ctx = malloc(sizeof(struct ui_ctx));
	if (!ctx)
		FATAL_ERR("ui: failed to allocate ctx");

	ctx->r_ctx = vt.rendering_init();
	ctx->vt = vt;

	vt.rendering_ctx_log(ctx->r_ctx);

	pthread_mutex_init(&ctx->panes.lock, NULL);

	if (pthread_mutex_lock(&ctx->panes.lock) != 0)
		FATAL_ERR("couldn't lock newly created pane mutex");

	ctx->panes.count = 1;
	ctx->panes.panes[0].canvas = vt.canvas_init(ctx->r_ctx);
	ctx->panes.panes[0].name = strdup("root");

	if (!ctx->panes.panes[0].name)
		FATAL_ERR("ui: ctx_new: pane creation OOM");

	struct color bg = {
		.r = 0x3A,
		.g = 0x22,
		.b = 0xBD,
	};

	rendering_fill(ctx->panes.panes[0].canvas, bg);

	pthread_mutex_unlock(&ctx->panes.lock);

	// Make sure that the cancellation fd is invalid until ui_thread gets
	// around to making the pipe.
	ctx->cancellation_fd = -1;

	// sync_fd, on the other hand, can be initialized immediately.
	int sync_fds[2];
	if (pipe(sync_fds) != 0) {
		FATAL_ERR("pipe(2) failed for sync fd's: %s", STR_ERR);
	}

	ctx->sync_fd_rx = sync_fds[0];
	ctx->sync_fd_tx = sync_fds[1];

	return ctx;
}

void *ui_thread(void *arg)
{
	struct ui_ctx *ctx = arg;

	int cancellation_pipe[2];
	if (pipe(cancellation_pipe) != 0)
		FATAL_ERR("ui: pipe: %s", STR_ERR);

	ctx->cancellation_fd = cancellation_pipe[0];

	pthread_t pane_handle;
	pthread_create(&pane_handle, NULL, rotate_panes, ctx);

	term_block();
	if (write(cancellation_pipe[1], "\0", 1) != 1)
		FATAL_ERR("ui: failed to write to cancellation pipe");

	pthread_join(pane_handle, NULL);

	fprintf(stderr, "ui: terminating\n");

	if (pthread_mutex_lock(&ctx->panes.lock) != 0)
		FATAL_ERR("Failed to lock panes.");

	size_t len = ctx->panes.count;
	for (size_t i = 0; i < len; i++) {
		canvas_deinit(ctx->panes.panes[i].canvas);
		free(ctx->panes.panes[i].name);
	}

	ctx->vt.rendering_cleanup(ctx->r_ctx);
	free(ctx);

	return NULL;
}

void ui_sync(struct ui_ctx *ctx)
{
	char data[] = { 0 };
	if (write(ctx->sync_fd_tx, data, 1) != 1)
		fprintf(stderr, "failed to write to sync_fd_tx: %s", STR_ERR);
}

static void *rotate_panes(void *arg)
{
	int r;

	struct ui_ctx *ctx = arg;
	struct pollfd fds[2];
	fds[0].fd = ctx->cancellation_fd;
	fds[0].events = POLLIN;
	fds[1].fd = ctx->sync_fd_rx;
	fds[1].events = POLLIN;

	int sleep_time = PANE_DELAY;

	size_t idx_increment = 1;

	for (size_t i = 0;; i += idx_increment) {
		r = pthread_mutex_lock(&ctx->panes.lock);

		if (r != 0)
			FATAL_ERR("ui: couldn't take lock: %s", strerror(r));

		size_t idx = i % ctx->panes.count;
		struct pane *p = &ctx->panes.panes[idx];

		fprintf(stderr, "ui: flipping pane: %s\n", p->name);
		ctx->vt.rendering_show(ctx->r_ctx, p->canvas);

		r = pthread_mutex_unlock(&ctx->panes.lock);
		if (r != 0)
			FATAL_ERR("ui: couldn't return lock: %s", strerror(r));

		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC_RAW, &a); // TODO: error checking

		// Do a cancellable 1-second sleep.
		poll(fds, sizeof(fds) / sizeof(*fds), sleep_time);
		if (fds[0].revents &= POLLIN) {
			// cancellation fd
			break;
		} else if (fds[1].revents &= POLLIN) {
			idx_increment = 0;
			char buf[1];
			if (read(fds[1].fd, buf, 1) != 1) {
				fprintf(stderr,
				    "ui: rotate_panes: couldn't read from sync_fd.");
				continue;
			}

			clock_gettime(CLOCK_MONOTONIC_RAW, &b);
			int delta = (b.tv_sec - a.tv_sec) * 1000 +
			    (b.tv_nsec / 1000 / 1000) -
			    (a.tv_nsec / 1000 / 1000);

			sleep_time -= delta;
			if (sleep_time < 0) {
				sleep_time = PANE_DELAY;
				idx_increment = 1;
			}
		} else {
			idx_increment = 1;
			sleep_time = PANE_DELAY;
		}
	}

	return NULL;
}

static struct pane *lookup_pane_thread_unsafe(
    struct pane_storage *panes, const char *name)
{
	for (size_t i = 0; i < panes->count; i++) {
		struct pane *p = &panes->panes[i];
		if (strcmp(p->name, name) == 0)
			return p;
	}
	return NULL;
}

/* Create a pane. */
enum ui_failure ui_pane_create(
    struct ui_ctx *ctx, char *name, struct color fill)
{
	// Really huge assumption that only one thread is calling into this or
	// the deletion function at once.
	pthread_mutex_lock(&ctx->panes.lock);

	size_t idx = ctx->panes.count;
	if (lookup_pane_thread_unsafe(&ctx->panes, name)) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_DUPLICATE;
	}

	if (idx >= MAX_PANES) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_TOO_MANY_PANES;
	}
	struct pane *p = &ctx->panes.panes[idx];

	p->name = strdup(name);
	if (!p->name) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_OOM;
	}

	p->canvas = ctx->vt.canvas_init(ctx->r_ctx);
	if (!p->canvas) {
		free(p->name);
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_OOM;
	}

	rendering_fill(p->canvas, fill);

	ctx->panes.count++;
	pthread_mutex_unlock(&ctx->panes.lock);

	return UI_OK;
}

enum ui_failure ui_pane_remove(struct ui_ctx *ctx, char *name)
{
	int r;

	r = pthread_mutex_lock(&ctx->panes.lock);
	if (r != 0)
		FATAL_ERR(
		    "ui_pane_remove: failed to take lock: %s", strerror(r));

	for (size_t i = 0; i < ctx->panes.count; i++) {
		if (strcmp(ctx->panes.panes[i].name, name) != 0)
			continue;

		struct pane *p = &ctx->panes.panes[i];
		free(p->name);
		canvas_deinit(p->canvas);

		for (size_t j = i + 1; j < ctx->panes.count; j++) {
			struct pane *q = &ctx->panes.panes[j];
			p->name = q->name;
			p->canvas = q->canvas;

			p = q;
		}

		ctx->panes.count--;

		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_OK;
	}

	pthread_mutex_unlock(&ctx->panes.lock);
	return UI_NO_SUCH_PANE;
}

enum ui_failure ui_pane_draw_shape(
    struct ui_ctx *ctx, char *name, const void *shape, render_fn_t inner)
{
	int r;

	r = pthread_mutex_lock(&ctx->panes.lock);
	if (r != 0)
		FATAL_ERR(
		    "ui_pane_draw_shape: failed to lock: %s\n", strerror(r));

	struct pane *p = lookup_pane_thread_unsafe(&ctx->panes, name);
	if (!p) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_NO_SUCH_PANE;
	}

	inner(p->canvas, shape);

	pthread_mutex_unlock(&ctx->panes.lock);
	return UI_OK;
}

enum ui_failure ui_pane_save(struct ui_ctx *ctx, char *name, char *path)
{
	// `name` is the pane whose canvas we are saving, not The Target. the
	// target should always be "root" because this is a privileged action.

	int r;

	r = pthread_mutex_lock(&ctx->panes.lock);
	if (r != 0)
		FATAL_ERR("%s: failed to lock: %s\n", __func__, strerror(r));

	struct pane *p = lookup_pane_thread_unsafe(&ctx->panes, name);
	if (!p) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_NO_SUCH_PANE;
	}

	const char *dirpath = ".";

	DIR *dir = opendir(dirpath);
	if (!dir)
		FATAL_ERR("Failed to open directory: %s: %s", dirpath, STR_ERR);

	rendering_dump_bgra_to_rgba(p->canvas, dir, dirpath, path);
	fprintf(stderr, "ui: saved pane '%s' RGBA pixel data to file: %s/%s\n",
	    name, dirpath, path);

	pthread_mutex_unlock(&ctx->panes.lock);
	return UI_OK;
}
