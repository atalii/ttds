#include "input.h"

#include "../abort.h"
#include "threads/termination.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <systemd/sd-bus-protocol.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define MAX_INPUTS 32

struct session {
	char *name;
	char *id;
};

struct sessions {
	size_t n;
	struct session *sessions;
};

struct eloop_ctx {
	struct sessions sessions;
	struct sd_bus *bus;
	char *seat;
	long cancellation_fd;
	size_t numfds;
	struct pollfd fds[MAX_INPUTS + 1];
};

static void *arg_use = NULL;

static char *read_seat(sd_bus *);
static struct sessions read_sessions(sd_bus *, const char *);
static void free_sessions(struct sessions);
static void switch_to_tty(struct eloop_ctx *, uint32_t id);

/* Read through ctx->fds. Return true iff the event loop should quit. */
static bool scan_poll(struct eloop_ctx *);
static void *event_loop(void *arg);

/* Iterate through /dev/input and put all the requisite fd's (including
 * cancellation) into eloop_ctx. */
static void gather_fds(struct eloop_ctx *);

static void cleanup(struct eloop_ctx *);

void *drm_input_thread(void *arg)
{
	arg_use = arg; // shut clang up :)

	// make sure we don't grab a key while it's pressed
	const struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = 1000,
	};

	nanosleep(&ts, NULL);

	// Assume only 1 seat and that sessions are never created or destroyed.
	// This latter assumption will break in fun and silly ways.

	int r;
	sd_bus *bus = NULL;

	r = sd_bus_default_system(&bus);
	if (r < 0)
		FATAL_ERR("Couldn't open dbus: %s", strerror(-r));

	char *seat = read_seat(bus);
	struct sessions sessions = read_sessions(bus, seat);

	int cancellation_pipe[2];
	if (pipe(cancellation_pipe) != 0)
		FATAL_ERR("loading pipe failed: %s", STR_ERR);

	struct eloop_ctx ctx = {
		.sessions = sessions,
		.bus = bus,
		.seat = seat,
		.cancellation_fd = cancellation_pipe[0],
	};

	gather_fds(&ctx);

	pthread_t eloop_thread;
	pthread_create(&eloop_thread, NULL, event_loop, &ctx);

	term_block();

	uint8_t data = 1;
	if (write(cancellation_pipe[1], &data, 1) != 1)
		FATAL_ERR("Failed to write to input cancellation pipe.");

	pthread_join(eloop_thread, NULL);

	switch_to_tty(&ctx, 3);
	cleanup(&ctx);

	return NULL;
}

static char *read_seat(sd_bus *bus)
{
	int r;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;

	r = sd_bus_call_method(bus, "org.freedesktop.login1",
	    "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
	    "ListSeats", &err, &reply, "");

	if (r < 0)
		FATAL_ERR("Call to ListSeats failed: %s", strerror(-r));

	char *name = NULL;
	char *path = NULL;

	r = sd_bus_message_read(reply, "a(so)", 1, &name, &path);
	if (r < 0)
		FATAL_ERR("Couldn't read ListSeats reply: %s", strerror(-r));

	name = strdup(name); // take out of sd_bus's refcount system.
	sd_bus_message_unref(reply);

	return name;
}

static struct sessions read_sessions(sd_bus *bus, const char *target_seat)
{
	struct sessions ret = {
		.n = 0,
		.sessions = NULL,
	};

	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int r;

	r = sd_bus_call_method(bus, "org.freedesktop.login1",
	    "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
	    "ListSessionsEx", &err, &reply, "");

	if (r < 0)
		FATAL_ERR("Couldn't call ListSessionsEx: %s", strerror(-r));

	r = sd_bus_message_enter_container(
	    reply, SD_BUS_TYPE_ARRAY, "(sussussbto)");

	if (r < 0)
		FATAL_ERR("Couldn't enter container from ListSessionsEx: %s",
		    strerror(-r));

	for (;;) {
		char *id = NULL;
		char *seat = NULL;
		char *name = NULL;

		r = sd_bus_message_read(reply, "(sussussbto)", &id, NULL, NULL,
		    &seat, NULL, NULL, &name, NULL, NULL, NULL);

		if (r < 0)
			FATAL_ERR("Couldn't read session: %s", strerror(-r));

		if (r == 0)
			break;

		if (strcmp(seat, target_seat) != 0)
			continue;

		ret.n++;
		ret.sessions =
		    realloc(ret.sessions, sizeof(struct session) * ret.n);

		struct session *target = &ret.sessions[ret.n - 1];

		if (!ret.sessions)
			FATAL_ERR("OOM while reading sessions.");

		target->name = strdup(name);
		target->id = strdup(id);
		if (!target->name || !target->id)
			FATAL_ERR("OOM while writing session metadata.");
	}

	sd_bus_message_unref(reply);
	return ret;
}

static void free_sessions(struct sessions sessions)
{
	for (size_t i = 0; i < sessions.n; i++) {
		struct session *target = &sessions.sessions[i];
		free(target->id);
		free(target->name);
	}

	free(sessions.sessions);
}

static void switch_to_tty(struct eloop_ctx *ctx, uint32_t id)
{
	int r;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;

	char target_name[14];
	sprintf(target_name, "tty%d", id);

	char *sess_id = NULL;

	for (size_t i = 0; i < ctx->sessions.n; i++) {
		struct session *session = &ctx->sessions.sessions[i];
		if (strcmp(session->name, target_name) == 0) {
			sess_id = session->id;
			break;
		}
	}

	if (!sess_id) {
		fprintf(stderr, "No such session exists.\n");
		goto cleanup;
	}

	r = sd_bus_call_method(ctx->bus, "org.freedesktop.login1",
	    "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
	    "ActivateSession", &err, &reply, "s", sess_id);

	if (r < 0)
		FATAL_ERR("Can't activate session: %s", strerror(-r));

cleanup:
	sd_bus_message_unref(reply);
}

static bool scan_poll(struct eloop_ctx *ctx)
{
	for (size_t i = 0; i < ctx->numfds; i++) {
		if (!(ctx->fds[i].revents &= POLLIN))
			continue;

		ctx->fds[i].revents = 0;

		if (ctx->fds[i].fd == ctx->cancellation_fd)
			return true;

		struct input_event event;
		int r = read(ctx->fds[i].fd, &event, sizeof(event));
		if (r < 0) {
			fprintf(stderr, "input: failed to read event: %s\n",
			    STR_ERR);
			continue;
		}

		if (r != sizeof(event))
			FATAL_ERR("Couldn't fill event buffer.");

		if (event.type != EV_KEY)
			continue;

		if (event.value != EV_KEY) // release
			continue;

		if (event.code == KEY_SPACE) {
			kill(getpid(), SIGINT);
			continue;
		}

		if (event.code >= KEY_1 && event.code <= KEY_0) {
			uint32_t target =
			    event.code == KEY_0 ? 0 : event.code - KEY_1 + 1;

			switch_to_tty(ctx, target);
		}
	}

	return false;
}

static void *event_loop(void *arg)
{
	struct eloop_ctx *ctx = arg;
	for (;;) {
		int r = poll(ctx->fds, ctx->numfds, 5000);
		if (r < 0)
			FATAL_ERR("Event poll failed: %s", STR_ERR);

		if (scan_poll(ctx))
			break;
	}

	for (size_t i = 0; i < ctx->numfds; i++) {
		ioctl(ctx->fds[i].fd, EVIOCGRAB, 0);
	}

	return NULL;
}

static void gather_fds(struct eloop_ctx *ctx)
{
	int fd = open("/dev/input", O_RDONLY);
	if (fd == -1)
		FATAL_ERR("Failed to open(2) /dev/input: %s", STR_ERR);

	DIR *inputs = fdopendir(fd);
	if (!inputs)
		FATAL_ERR("Couldn't fdopendir(3) /dev/input: %s", STR_ERR);

	ctx->numfds = 0;

	struct dirent *dir;

	for (;;) {
		if (ctx->numfds >= MAX_INPUT)
			FATAL_ERR("Too many inputs.");

		errno = 0;
		dir = readdir(inputs);
		if (!dir && errno == 0) {
			break;
		} else if (!dir) {
			FATAL_ERR("Couldn't readdir(3): %s", STR_ERR);
		}

		// We could also check if d_type is DT_CHR and skip as necessary
		// (or, more reliably, use stat(2)). However, there are two
		// reasons not to:
		//
		//   1. I'm not sure what feature test macros need to be in
		//   place for DT_CHR, and I'm not sure if they're reliably
		//   supported.
		//
		//   2. The upcoming EVIOCGRAB ioctl will fail on invalid
		//   entries anyway.

		int ev_file_fd = openat(fd, dir->d_name, O_RDONLY);
		if (ev_file_fd == -1) {
			fprintf(stderr, "Failed to open dev: %s: %s\n",
			    dir->d_name, STR_ERR);
			continue;
		}

		if (ioctl(ev_file_fd, EVIOCGRAB, 1) != 0) {
			fprintf(stderr, "EVIOCGRAB failed on: %s: %s\n",
			    dir->d_name, STR_ERR);

			close(ev_file_fd);
			continue;
		}

		struct pollfd *target = &ctx->fds[(ctx->numfds)++];
		target->fd = ev_file_fd;
		target->events = POLLIN;
		target->revents = 0;
	}

	ctx->fds[ctx->numfds].fd = ctx->cancellation_fd;
	ctx->fds[ctx->numfds].events = POLLIN;
	ctx->fds[ctx->numfds].revents = 0;

	ctx->numfds++;

	closedir(inputs);
}

static void cleanup(struct eloop_ctx *ctx)
{
	sd_bus_close_unref(ctx->bus);
	free_sessions(ctx->sessions);
	free(ctx->seat);
}
