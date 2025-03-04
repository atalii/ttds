#include "abort.h"
#include "rendering/rendering.h"
#include "testing.h"
#include "threads/commands.h"
#include "threads/termination.h"
#include "threads/ui.h"

#include <assert.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define SPAWN_THREAD(thread, handle, arg)                                   \
	pthread_t handle;                                                   \
	do {                                                                \
		if (pthread_create(&handle, NULL, thread, arg) != 0)        \
			FATAL_ERR("Couldn't spawn thread: " #thread ": %s", \
			    STR_ERR);                                       \
	} while (0);

struct backend_opt {
	enum backend backend;
	const char *as_string;
	const char *help;
};

struct args {
	enum backend backend;

	// If non-null, run the tests and dump pixel buffers here.
	char *tests_dump_dir;
};

static void print_usage(const char *);
static struct args parse_args(int argc, char **argv);

const struct backend_opt backend_strings[] = {
	// The first backend is treated as the default.
	{ BACKEND_DRM, "DRM", "directly use the Linux DRM subsystem" },
	{ BACKEND_MEM, "MEM", "drawing uses zero I/O, only happens in-memory" },
};

int main(int argc, char *argv[])
{
	// When running under webproc[1] for debugging, line buffering needs to
	// be enabled explicitly. Otherwise, we don't get logs when we think we
	// should get logs.
	//
	// [1]: https://github.com/atalii/webproc
	setvbuf(stdout, NULL, _IOLBF, 32);
	setvbuf(stderr, NULL, _IOLBF, 32);

	assert(backend_count ==
	    (sizeof(backend_strings) / sizeof(*backend_strings)));

	// Parse command-line arguments. This only returns if they're valid.
	const struct args args = parse_args(argc, argv);
	const struct rendering_vtable vt = supported_backends[args.backend];

	// We might be asked to run the test cases. This isn't interactive, it
	// just dumps the pixel buffers into a bespoke directory for diffing.
	if (args.tests_dump_dir != NULL) {
		run_tests(args.tests_dump_dir);
		return 0;
	}

	term_init(4);

	// Prepare to block for SIGINT.
	sigset_t f, b;
	sigemptyset(&f);
	sigaddset(&f, SIGINT);
	sigprocmask(SIG_BLOCK, &f, &b);

	// Spawn child threads.
	struct ui_ctx *ui_ctx = ui_ctx_new(vt);
	SPAWN_THREAD(ui_thread, ui_handle, ui_ctx);
	SPAWN_THREAD(vt.input_thread, input_handle, NULL);
	SPAWN_THREAD(cmd_thread, cmd_handle, ui_ctx);

	// Block for SIGINT.
	for (int s = 0; s != SIGINT; sigwait(&f, &s)) {
	}

	fprintf(stderr, "SIGINT received; cleaning up.\n");
	term();

	pthread_join(input_handle, NULL);
	pthread_join(ui_handle, NULL);
	pthread_join(cmd_handle, NULL);

	return 0;
}

static void print_usage(const char *self)
{
	fprintf(stderr, "Usage: %s [OPTION]...\n", self);

	// Generate help text for --backend flag.
	fprintf(stderr, "      --backend <BACKEND>\n");
	fprintf(stderr, "  \tSet the rendering backend to use.\n");
	for (size_t i = 0; i < backend_count; i++) {
		struct backend_opt opt = backend_strings[i];
		fprintf(stderr, "  \t  - \"%s\"%s: %s\n", opt.as_string,
		    i == 0 ? " (default)" : "", opt.help);
	}

	fprintf(stderr, "      --test <DIR>\n");
	fprintf(stderr,
	    "  \tRun the test cases and dump their pixel buffers into <DIR>.\n");
	fprintf(stderr,
	    "  \tThese can be manually diffed to verify the rendering code.\n");

	fprintf(stderr, "  -h, --help\n");
	fprintf(stderr, "  \tPrint help.\n");
}

static struct args parse_args(int argc, char **argv)
{
	char *const self = argc > 0 ? argv[0] : "ttds";

	// Initialize with default arguments.
	struct args args = {
		.backend = backend_strings[0].backend,
		.tests_dump_dir = NULL,
	};

	// i miss https://github.com/clap-rs/clap 💔

	while (true) {
		int option_index = 0;
		const struct option long_options[] = {
			{ "help", 0, NULL, 'h' },
			{ "backend", required_argument, NULL, 'b' },
			{ "test", required_argument, NULL, 't' },
			{ 0 }, // this must be terminated with some end
			       // indicator since getopt does not accept a
			       // length
		};

		int c =
		    getopt_long(argc, argv, "h", long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_usage(self);
			exit(0);
		case 'b':
			for (size_t i = 0; i < backend_count; i++) {
				struct backend_opt opt = backend_strings[i];
				if (strcasecmp(optarg, opt.as_string) == 0) {
					args.backend = opt.backend;
					goto found_backend;
				}
			}
			fprintf(stderr, "%s: unrecognized backend '%s'\n", self,
			    optarg);
			exit(1);
		found_backend:
			break;
		case 't':
			args.tests_dump_dir = optarg;
			break;
		case '?':
			exit(1);
		default:
			assert(false && "unreachable");
			abort();
		}
	}

	if (optind != argc) {
		fprintf(stderr, "too many arguments to %s.\n", self);
		exit(1);
	}

	return args;
}
