#include "testing.h"

#include "abort.h"
#include "rendering/canvas.h"

#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PI 3.141592653589793238462
#define TAU (2. * PI)

static inline intmax_t min(intmax_t a, intmax_t b);

static const struct color BG = { .r = 0x3A, .g = 0x22, .b = 0xBD };
static const struct color FG = { .r = 0xFF, .g = 0xFF, .b = 0xFF };

struct test {
	struct color fill_color;
	void (*draw_fn)(struct canvas *);
	const char *output_path;
	uint16_t width, height;
};

static void run_these_tests(
    const char *dump_dir_path, const struct test tests[], size_t num_tests);

static void test_rects(struct canvas *c);
static void test_circles(struct canvas *c);
static void test_lines_burst(struct canvas *c);

void run_tests(const char *dump_dir)
{
	struct test tests[] = {
		{
		    .fill_color = BG,
		    .draw_fn = test_rects,
		    .output_path = "rects.data",
		    .width = 32,
		    .height = 32,
		},
		{
		    .fill_color = BG,
		    .draw_fn = test_circles,
		    .output_path = "circles.data",
		    .width = 128,
		    .height = 128,
		},
		{
		    .fill_color = BG,
		    .draw_fn = test_lines_burst,
		    .output_path = "lines-burst.data",
		    .width = 32,
		    .height = 32,
		},
	};

	run_these_tests(dump_dir, tests, sizeof(tests) / sizeof(*tests));
}

static inline intmax_t min(intmax_t a, intmax_t b)
{
	return a < b ? a : b;
}

static void run_these_tests(
    const char *dump_dir_path, const struct test tests[], size_t num_tests)
{
	// Open (or create) the dump directory. Test output paths are relative
	// to this.
	if (mkdir(dump_dir_path,
		S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH |
		    S_IXOTH) < 0) {
		if (errno != EEXIST)
			FATAL_ERR("Failed to create directory: %s: %s",
			    dump_dir_path, STR_ERR);
	}
	DIR *dump_dir = opendir(dump_dir_path);

	if (dump_dir == NULL)
		FATAL_ERR(
		    "Failed to open directory: %s: %s", dump_dir_path, STR_ERR);

	for (size_t i = 0; i < num_tests; i++) {
		// Initialize in-memory canvas.
		struct canvas *canvas =
		    canvas_init_bgra(tests[i].width, tests[i].height);
		if (canvas == NULL)
			FATAL_ERR("out of memory");

		// Clear canvas.
		rendering_fill(canvas, tests[i].fill_color);

		// Run the test.
		(tests[i].draw_fn)(canvas);

		// Save canvas as raw RGBA pixel data.
		rendering_dump_bgra_to_rgba(
		    canvas, dump_dir, dump_dir_path, tests[i].output_path);
		fprintf(stderr, "Wrote RGBA pixel data to file: %s/%s\n",
		    dump_dir_path, tests[i].output_path);

		canvas_deinit(canvas);
	}

	if (closedir(dump_dir) != 0)
		FATAL_ERR("couldn't close dir: %s", STR_ERR);
}

static void test_rects(struct canvas *c)
{
	// Draw tiny glider.
	const uint16_t glider[5][2] = {
		{ 1, 0 },
		{ 0, 1 },
		{ 0, 2 },
		{ 1, 2 },
		{ 2, 2 },
	};
	const uint16_t x0 = c->width * 7 / 10;
	const uint16_t y0 = c->height / 2;
	for (size_t i = 0; i < sizeof(glider) / sizeof(*glider); i++) {
		uint16_t size = 2;
		uint16_t x = x0 + glider[i][0] * size;
		uint16_t y = y0 + glider[i][1] * size;
		struct rect rect = {
			.w = size, .h = size, .x = x, .y = y, .c = FG
		};
		rendering_draw_rect(c, &rect);
	}

	// Draw some rectangles.
	const struct rect rects[] = {
		{ 0 }, // infinitely thin rectangle, no effect
		{ .x = c->width / 5,
		    .y = c->height * 2 / 3,
		    .w = c->width / 3,
		    .h = c->height / 4,
		    .c = FG },
		{ .x = c->width / 4,
		    .y = c->height / 3,
		    .w = c->width / 7,
		    .h = 1,
		    .c = FG },
		{ .x = c->width / 2,
		    .y = c->height * 2 / 7,
		    .w = c->width / 9,
		    .h = c->height / 6,
		    .c = FG },
	};
	for (size_t i = 0; i < sizeof(rects) / sizeof(*rects); i++) {
		rendering_draw_rect(c, &rects[i]);
	}
}

static void test_circles(struct canvas *c)
{
	const double gr = (sqrt(5.) - 1.) / 2.;

	// No-op.
	rendering_draw_circle(c, &(struct circle) { 0 });

	// Draw some circles imitating a sunflower!
	const size_t rmax = 3;
	const double distance = rmax * 2.2;

	const size_t circle_count = PI *
	    pow((min(c->width, c->height) - rmax) / 2., 2.) /
	    pow(distance + rmax, 2.);
	for (size_t i = 1; i <= circle_count; i++) {
		double p = (double)i / circle_count;
		double px = sqrt(p) * cos(TAU * gr * i);
		double py = sqrt(p) * sin(TAU * gr * i);
		double x = (1. + px) * ((double)c->width / 2. - rmax) + rmax;
		double y = (1. + py) * ((double)c->height / 2. - rmax) + rmax;
		struct circle circle = {
			.x = round(x),
			.y = round(y),
			.r = round(1. + rmax * p),
			.c = FG,
		};

		rendering_draw_circle(c, &circle);
	}
}

static void test_lines_burst(struct canvas *c)
{
	for (uint16_t x = 0; x <= c->width; x += c->width / 4) {
		for (uint16_t y = 0; y <= c->height; y += c->height) {
			struct line line = { .x0 = c->width / 2,
				.y0 = c->height / 2,
				.x1 = x,
				.y1 = y,
				.c = FG };
			rendering_draw_line(c, &line);
		}
	}
	for (uint16_t x = 0; x <= c->width; x += c->width) {
		for (uint16_t y = 0; y <= c->height; y += c->height / 4) {
			struct line line = { .x0 = c->width / 2,
				.y0 = c->height / 2,
				.x1 = x,
				.y1 = y,
				.c = FG };
			rendering_draw_line(c, &line);
		}
	}
}
