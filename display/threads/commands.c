#include "../abort.h"
#include "rendering/canvas.h"
#include "termination.h"
#include "ui.h"

#include <ctype.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_CMD_LEN 1024

typedef char *(*act_t)(struct ui_ctx *, char *target, size_t argc, char **argv);

struct command {
	char *target_name;
	char *action;
	size_t argc;
	char **argv;
};

struct parse_result {
	bool ok;
	union {
		struct command command;
		char *err;
	} val;
};

struct cmd_ctx {
	struct ui_ctx *ui_ctx;
	int cancellation_fd;
};

struct action_container {
	char *name;
	act_t hook;
};

static void *cmd_inner(void *arg);
static char *find_target(char **);
static bool parse(char **input_cursor, struct parse_result *result);

static char *call_action(struct ui_ctx *ctx, const struct command *c,
    const struct action_container *, size_t len, bool *found);

static char *run(struct ui_ctx *, const struct command *);

static char *eat_whitespace(char *);
static char **collect_args(char **, size_t *, char **);

static char *act_create(
    struct ui_ctx *, char *target, size_t argc, char **argv);
static char *act_remove(
    struct ui_ctx *, char *target, size_t argc, char **argv);
static char *act_rect(struct ui_ctx *, char *target, size_t argc, char **argv);
static char *act_circle(
    struct ui_ctx *, char *target, size_t argc, char **argv);
static char *act_line(struct ui_ctx *, char *target, size_t argc, char **argv);
static char *act_copy_rect(
    struct ui_ctx *, char *target, size_t argc, char **argv);
static char *act_bezier2(
    struct ui_ctx *, char *target, size_t argc, char **argv);

static char *act_term(struct ui_ctx *, char *target, size_t argc, char **argv);

static bool parse_color(const char *in, struct color *out);
static char *parse_args(const char *fmt, size_t argc, char **argv, ...);

static const struct action_container actions[] = {
	{ "CREATE", act_create },
	{ "REMOVE", act_remove },
	{ "RECT", act_rect },
	{ "CIRCLE", act_circle },
	{ "LINE", act_line },
	{ "COPY_RECT", act_copy_rect },
	{ "BEZIER2", act_bezier2 },
};

static const struct action_container root_actions[] = {
	{ "TERMINATE", act_term },
};

void *cmd_thread(void *arg)
{
	struct cmd_ctx ctx;

	int cancellation_pipe[2];
	if (pipe(cancellation_pipe) != 0)
		FATAL_ERR("commands: can't create cancellation pipe");

	// TODO: print buffer dimensions to stdout in JSON format
	ctx.ui_ctx = (struct ui_ctx *)arg;
	ctx.cancellation_fd = cancellation_pipe[0];

	pthread_t reader;
	if (pthread_create(&reader, NULL, cmd_inner, &ctx) != 0) {
		FATAL_ERR("commands: failed to spawn thread: %s", STR_ERR);
	}

	term_block();

	uint8_t data = 0;
	if (write(cancellation_pipe[1], &data, 1) != 1) {
		fprintf(stderr, "commands: failed to cancel reader thread\n");
	} else {
		pthread_join(reader, NULL);
	}

	return NULL;
}

static void *cmd_inner(void *arg)
{
	struct cmd_ctx *ctx = (struct cmd_ctx *)arg;
	int cancellation_fd = ctx->cancellation_fd;

	struct pollfd fds[2];

	fds[0].fd = cancellation_fd;
	fds[0].events = POLLIN;

	fds[1].fd = 0; // stdin
	fds[1].events = POLLIN;

	char line[MAX_CMD_LEN];

	for (;;) {
		if (poll(fds, 2, -1) < 0)
			FATAL_ERR("commands: poll failed: %s", STR_ERR);

		if (fds[0].revents &= POLLIN)
			break;

		if (!(fds[1].revents & POLLIN))
			continue;

		if (!fgets(line, MAX_CMD_LEN, stdin))
			FATAL_ERR(
			    "commands: couldn't read from stdin: %s", STR_ERR);

		// Trim newline.
		size_t len = strlen(line);
		if (len >= 1)
			line[len - 1] = '\0';

		char *line_cursor = line;
		char *target = find_target(&line_cursor);
		if (!target) {
			printf("target missing in command.\n");
			continue;
		}

		struct parse_result r = { 0 };
		bool failed = false;
		while (parse(&line_cursor, &r)) {
			if (!r.ok) {
				failed = true;
				printf("parsing failed: %s\n", r.val.err);
				free(r.val.err);
				continue;
			}

			r.val.command.target_name = target;
			char *err = run(ctx->ui_ctx, &r.val.command);

			if (err) {
				failed = true;
				printf("%s\n", err); // write to the client
				fprintf(stderr, "cmd: run: %s\n",
				    err); // write to the debug log

				break;
			}
		}

		if (!failed)
			printf("OK\n");

		ui_sync(ctx->ui_ctx);
	}

	return NULL;
}

static char *find_target(char **input_cursor)
{
	char *target_end = *input_cursor;
	for (; *target_end != ':' && *target_end != '\0'; target_end++)
		;

	if (target_end == *input_cursor || *target_end == '\0') {
		return NULL;
	}

	*target_end = '\0';
	char *target = *input_cursor;

	if (*target == '\0')
		return NULL;

	*input_cursor = target_end + 1;

	return target;
}

static bool parse(char **input_cursor, struct parse_result *result)
{
	// Commands look like this:
	// <target>: <action> <args>...[; <action> <args>...]*
	//
	// These can be separated by as many ASCII whitespace characters as
	// desired. Don't use broader UTF-8 whitespace; I'll cry.
	//
	// We just get this here after parse_target.
	*input_cursor = eat_whitespace(*input_cursor);
	if (**input_cursor == '\0')
		return false;

	char *action_end = *input_cursor;
	while (*action_end != '\0' && !isspace(*action_end))
		action_end++;

	result->ok = true;

	char *arg_start = &action_end[1];
	if (*action_end != '\0' && *action_end != ';') {
		*action_end = '\0';
		result->val.command.argc = 0;
		result->val.command.argv =
		    collect_args(&arg_start, &result->val.command.argc, NULL);
	}

	result->val.command.action = *input_cursor;
	*input_cursor = arg_start;

	return result;
}

static char *call_action(struct ui_ctx *ctx, const struct command *c,
    const struct action_container *candidates, size_t len, bool *found)
{
	for (size_t i = 0; i < len; i++) {
		if (strcmp(c->action, candidates[i].name) == 0) {
			*found = true;

			char *err_buf = candidates[i].hook(
			    ctx, c->target_name, c->argc, c->argv);

			if (c->argv)
				free(c->argv);

			return err_buf;
		}
	}

	*found = false;

	char *ret = malloc(512);
	snprintf(ret, 512, "no such action found: %s", c->action);
	return ret;
}

static char *run(struct ui_ctx *ctx, const struct command *c)
{
	char *ret = NULL;
	bool found;

	if (strcmp(c->target_name, "root") == 0) {
		char *root_ret = call_action(ctx, c, root_actions,
		    sizeof(root_actions) / sizeof(*root_actions), &found);
		if (found)
			return root_ret;
	}

	ret = call_action(
	    ctx, c, actions, sizeof(actions) / sizeof(*actions), &found);
	return ret;
}

static char *eat_whitespace(char *x)
{
	while (*x != '\0' && isspace(*x))
		x++;

	return x;
}

static char **collect_args(char **cursor, size_t *argc, char **argv)
{
	char *input = eat_whitespace(*cursor);
	if (*input == '\0' || *input == ';') {
		*input = '\0';
		*cursor = input;
		return argv;
	}

	char *input_end = input;
	while (!isspace(*input_end) && *input_end != '\0' && *input_end != ';')
		input_end++;

	(*argc)++;
	size_t alloc_size =
	    *argc * sizeof(char *); // TODO: FATAL_ERR on overflow
	argv = realloc(argv, alloc_size);
	if (!argv)
		FATAL_ERR("commands: collect_args: OOM");

	argv[*argc - 1] = input;

	if (*input_end == '\0') {
		*cursor = input_end;
		return argv;
	} else if (*input_end == ';') {
		*input_end = '\0';
		*cursor = input_end + 1;
		return argv;
	} else {
		*input_end = '\0';
		*cursor = input_end + 1;
		return collect_args(cursor, argc, argv);
	}
}

static char *act_create(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	char *err_buf = NULL;
	struct color fill;
	if ((err_buf = parse_args("c", argc, argv, &fill)))
		return err_buf;

	enum ui_failure r = ui_pane_create(ctx, target, fill);
	if (r != UI_OK) {
		err_buf = malloc(1024);
		snprintf(
		    err_buf, 1024, "act_create: failed: %s", ui_failure_str(r));
	}

	return err_buf;
}

static char *act_remove(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	char *err_buf = NULL;
	if ((err_buf = parse_args("", argc, argv)))
		return err_buf;

	enum ui_failure r = ui_pane_remove(ctx, target);
	if (r != UI_OK) {
		err_buf = malloc(1024);
		snprintf(
		    err_buf, 1024, "act_remove: failed: %s", ui_failure_str(r));
	}

	return err_buf;
}

static char *act_rect(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	char *err_buf = NULL;
	struct rect rect;

	long x, y, w, h;

	if ((err_buf =
		    parse_args("ciiii", argc, argv, &rect.c, &x, &y, &w, &h)))
		return err_buf;

	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;

	enum ui_failure r = ui_pane_draw_shape(
	    ctx, target, &rect, rendering_draw_rect_type_erased);

	if (r != UI_OK) {
		err_buf = malloc(1024);
		snprintf(
		    err_buf, 1024, "act_rect: failed: %s", ui_failure_str(r));
	}

	return err_buf;
}

static char *act_circle(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	char *err_buf = NULL;
	struct circle circle;

	long x, y, rad;

	if ((err_buf = parse_args("ciii", argc, argv, &circle.c, &x, &y, &rad)))
		return err_buf;

	circle.x = x;
	circle.y = y;
	circle.r = rad;

	enum ui_failure r = ui_pane_draw_shape(
	    ctx, target, &circle, rendering_draw_circle_type_erased);

	if (r != UI_OK) {
		err_buf = malloc(1024);
		snprintf(
		    err_buf, 1024, "act_circle: failed: %s", ui_failure_str(r));
	}

	return err_buf;
}

static char *act_line(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	char *err_buf = NULL;
	struct line line;

	long x0, y0, x1, y1;

	if ((err_buf = parse_args(
		 "ciiii", argc, argv, &line.c, &x0, &y0, &x1, &y1)))
		return err_buf;

	line.x0 = x0;
	line.y0 = y0;
	line.x1 = x1;
	line.y1 = y1;

	enum ui_failure r = ui_pane_draw_shape(
	    ctx, target, &line, rendering_draw_line_type_erased);

	if (r != UI_OK) {
		err_buf = malloc(1024);
		snprintf(
		    err_buf, 1024, "act_line: failed: %s", ui_failure_str(r));
	}

	return err_buf;
}

static char *act_copy_rect(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	char *err_buf = NULL;
	struct rect_copy rc;

	long dst_x, dst_y, src_x, src_y, w, h;

	if ((err_buf = parse_args(
		 "iiiiii", argc, argv, &dst_x, &dst_y, &src_x, &src_y, &w, &h)))
		return err_buf;

	rc.dst_x = dst_x;
	rc.dst_y = dst_y;
	rc.src_x = src_x;
	rc.src_y = src_y;
	rc.w = w;
	rc.h = h;

	enum ui_failure r = ui_pane_draw_shape(
	    ctx, target, &rc, rendering_draw_rect_copy_type_erased);

	if (r != UI_OK) {
		err_buf = malloc(1024);
		snprintf(err_buf, 1024, "act_copy_rect: failed: %s",
		    ui_failure_str(r));
	}

	return err_buf;
}

static char *act_bezier2(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	char *err_buf = NULL;
	struct bezier2 b;

	long x0, y0, x1, y1, x2, y2;

	if ((err_buf = parse_args(
		 "ciiiiii", argc, argv, &b.c, &x0, &y0, &x1, &y1, &x2, &y2)))
		return err_buf;

	b.x0 = x0;
	b.y0 = y0;
	b.x1 = x1;
	b.y1 = y1;
	b.x2 = x2;
	b.y2 = y2;

	enum ui_failure r = ui_pane_draw_shape(
	    ctx, target, &b, rendering_draw_bezier2_type_erased);

	if (r != UI_OK) {
		err_buf = malloc(1024);
		snprintf(err_buf, 1024, "act_bezier2: failed: %s",
		    ui_failure_str(r));
	}

	return err_buf;
}

static char *act_term(struct ui_ctx *, char *, size_t, char **)
{
	kill(getpid(), SIGINT);

	// We could be more attentive to error conditions here (e.g., if
	// getpid(2) or kill(2) fail), but I'm fixing this double-ssh'd into
	// mulaney, so that's a problem for much later.
	return strdup("Terminating.");
}

static bool parse_color(const char *in, struct color *out)
{
	if (in[0] != '#')
		return false;

	if (strlen(in) != 7)
		return false;

	errno = 0;
	long result = strtol(&in[1], NULL, 16);
	if (errno != 0)
		return false;

	out->r = result >> 16;
	out->g = result >> 8 & 0xff;
	out->b = result & 0xff;

	return true;
}

static char *parse_args(const char *fmt, size_t argc, char **argv, ...)
{
	va_list args;
	va_start(args, argv);
	char *err_buf = malloc(1024);
	if (!err_buf)
		FATAL_ERR("parse_args: OOM");

	if (argc != strlen(fmt)) {
		snprintf(err_buf, 1024,
		    "failure: got %zu arguments, expected %lu.", argc,
		    strlen(fmt));
		return err_buf;
	}

	for (size_t i = 0; *fmt != '\0'; fmt++, i++) {
		char c = *fmt;
		char *in = argv[i];

		switch (c) {
		case 'c':
			struct color *cout = va_arg(args, struct color *);
			if (!parse_color(in, cout)) {
				snprintf(err_buf, 1024,
				    "failure: expected color, got: %s", in);
				return err_buf;
			}

			break;
		case 'i':
			long *out = va_arg(args, long *);
			char *end = NULL;
			long iout = strtol(in, &end, 0);
			*out = iout;
			if (*end != '\0') {
				snprintf(err_buf, 1024,
				    "failure: expected number, got: %s", in);
				return err_buf;
			}
			break;
		default:
			// Violently explode in lieu of proper compile-time type
			// checks.
			FATAL_ERR("unrecognized fmt parameter: %c", c);
		}
	}

	va_end(args);

	free(err_buf);
	return NULL;
}
