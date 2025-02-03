#include "canvas.h"

#include "abort.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFN_RENDER(type, body)                                               \
	void rendering_draw_##type(struct canvas *c, const struct type *type) \
	{                                                                     \
		body                                                          \
	}                                                                     \
                                                                              \
	void rendering_draw_##type##_type_erased(                             \
	    struct canvas *c, const void *v)                                  \
	{                                                                     \
		rendering_draw_##type(c, v);                                  \
	}

static inline intmax_t max(intmax_t a, intmax_t b);
static void draw_point(struct canvas *, int32_t x, int32_t y, struct color);

struct canvas *canvas_init_bgra(uint16_t width, uint16_t height)
{
	// These Linux sources were helpful in understanding the intended
	// memory layout & alignment of these pixel buffers
	// (as of commit ffd294d346d185b70e28b1a28abe367bbfe53c04):
	// - linux/drivers/gpu/drm/drm_dumb_buffers.c:60
	// - linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c:942
	// - linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c:916

	struct canvas *ret = malloc(sizeof(struct canvas));
	if (ret == NULL)
		return NULL;

	ret->width = width;
	ret->height = height;
	ret->stride = width * 4;

	ret->buffer = calloc(ret->height, ret->stride);
	if (ret->buffer == NULL)
		return NULL;

	return ret;
}

void canvas_deinit(struct canvas *c)
{
	free(c->buffer);
	free(c);
}

void rendering_fill(struct canvas *c, struct color color)
{
	// Color space is little endian, thus the BGRA format used below:
	for (uint16_t x = 0; x < c->width; x++)
		for (uint16_t y = 0; y < c->height; y++)
			draw_point(c, x, y, color);
}

DEFN_RENDER(rect, {
	printf("%d, %d, %d\n", rect->c.r, rect->c.g, rect->c.b);

	uint16_t right_edge = rect->x + rect->w;
	uint16_t bottom_edge = rect->y + rect->h;
	for (uint16_t y = rect->y; y < bottom_edge && y < c->height; y++)
		for (uint16_t x = rect->x; x < right_edge && x < c->width; x++)
			draw_point(c, x, y, rect->c);
})

DEFN_RENDER(circle, {
	uint16_t x = circle->r;
	uint16_t y = 0;
	int32_t t1 = circle->r / 16;

	const struct color color = circle->c;

	while (x > 0 && x >= y) {
		int32_t eff_y = circle->y + y;
		for (int32_t eff_x = circle->x + x; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - y;
		for (int32_t eff_x = circle->x + x; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - y;
		for (int32_t eff_x = circle->x - x; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + y;
		for (int32_t eff_x = circle->x - x; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + x;
		for (int32_t eff_x = circle->x + y; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - x;
		for (int32_t eff_x = circle->x + y; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - x;
		for (int32_t eff_x = circle->x - y; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + x;
		for (int32_t eff_x = circle->x - y; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		y++;
		t1 = t1 + y;
		int32_t t2 = t1 - x;
		if (t2 >= 0) {
			t1 = t2;
			x--;
		}
	}
})

DEFN_RENDER(line, {
	const struct color color = line->c;

	// I think this is essentially Bresenham's line algorithm, as in, that
	// is where the ideas came from, but with the caveat that I didn't want
	// a separate vertical and horizontal drawing function.
	//
	// ## Terms
	//
	// Since this is octant-agnostic, we generalize the notion of a step
	// between pixels.
	//
	// Considering the vector (dx, dy), one component will have greater or
	// equal magnitude than the other, so at every iteration, the pixel
	// position will be incremented along that component. I refer to this
	// increment vector as the step.
	//
	// The pixel position may also be incremented along the component with
	// lesser magnitude, but this happens a fraction of the time (a fraction
	// in the range [0, 1]). I refer to this as the lesser step.
	//
	// ## Process for deriving
	//
	// 1. Determine whether to step in the lesser direction.
	//    - Given the two possible future points (either cur + step or cur +
	//      step + lstep), and the point on the line which lies between
	//      them, which future point is closer to the line? This calculation
	//      is made with real numbers, approximated with floats.
	// 2. Scale the two distances such that the comparison is still
	//    meaningful, while eliminating floating point division. Now, we can
	//    use integers!
	// 3. Avoid recomputing the distances from scratch each iteration.
	//    - Determine the initial value, and the difference between the
	//      (i+1)th and ith values for each branch.

	const int32_t dx = (int32_t)line->x1 - (int32_t)line->x0;
	const int32_t dy = (int32_t)line->y1 - (int32_t)line->y0;

	const uint16_t steps = (uint16_t)max(abs(dx), abs(dy));

	if (steps == 0) {
		draw_point(c, line->x0, line->y0, color);
		return;
	}

	// step_x and step_y are both in the range [-1, 1], and
	// at least one is -1 or 1
	const int32_t step_x = dx / steps;
	const int32_t step_y = dy / steps;

	const int32_t lstep_x = step_x == 0 ? (dx != 0 ? dx / abs(dx) : 0) : 0;
	const int32_t lstep_y = step_y == 0 ? (dy != 0 ? dy / abs(dy) : 0) : 0;

	uint16_t x = line->x0;
	uint16_t y = line->y0;

	int32_t d0_x = steps * step_x;
	int32_t d0_y = steps * step_y;
	int32_t d1_x = -steps * (step_x + lstep_x);
	int32_t d1_y = -steps * (step_y + lstep_y);

	for (uint16_t i = 0; i <= steps; i++) {
		draw_point(c, x, y, color);

		const int32_t px = abs(d0_x) - abs(d1_x);
		const int32_t py = abs(d0_y) - abs(d1_y);

		x += step_x;
		d0_x += steps * step_x - dx;
		d1_x -= steps * step_x - dx;
		if (px >= 0) {
			x += lstep_x;
			d0_x += steps * lstep_x;
			d1_x -= steps * lstep_x;
		}

		y += step_y;
		d0_y += steps * step_y - dy;
		d1_y -= steps * step_y - dy;
		if (py >= 0) {
			y += lstep_y;
			d0_y += steps * lstep_y;
			d1_y -= steps * lstep_y;
		}
	}
})

void rendering_dump_bgra_to_rgba(
    const struct canvas *c, DIR *dir, const char *dirpath, const char *path)
{
	const int fd = openat(dirfd(dir), path, O_RDWR | O_CREAT | O_TRUNC,
	    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0)
		FATAL_ERR("Failed to open file for writing: %s/%s: %s", dirpath,
		    path, STR_ERR);

	const size_t buffer_size = (size_t)c->height * (size_t)c->stride;
	if (ftruncate(fd, buffer_size) < 0)
		FATAL_ERR("Failed to grow file to %zu bytes: %s/%s: %s",
		    buffer_size, dirpath, path, STR_ERR);

	uint8_t *const dst =
	    mmap(NULL, buffer_size, PROT_WRITE, MAP_SHARED, fd, 0);
	if (dst == MAP_FAILED)
		FATAL_ERR("Failed to mmap file for writing: %s/%s: %s", dirpath,
		    path, STR_ERR);

	for (uint16_t y = 0; y < c->height; y++) {
		for (uint16_t x = 0; x < c->width; x++) {
			const size_t idx = (c->stride * y) + (x * 4);
			const uint8_t rgba_pixel[4] = {
				c->buffer[idx + 2], // R
				c->buffer[idx + 1], // G
				c->buffer[idx + 0], // B
				c->buffer[idx + 3], // A
			};

			memcpy(&dst[idx], rgba_pixel, sizeof(rgba_pixel));
		}
	}

	if (munmap(dst, buffer_size) < 0)
		FATAL_ERR("Failed to unmap file contents: %s/%s: %s", dirpath,
		    path, STR_ERR);

	if (close(fd) < 0)
		FATAL_ERR(
		    "Failed to close file: %s/%s: %s", dirpath, path, STR_ERR);
}

static inline intmax_t max(intmax_t a, intmax_t b)
{
	return a < b ? b : a;
}

static void draw_point(
    struct canvas *c, int32_t x, int32_t y, struct color color)
{
	// Drawing a point out of bounds is a no-op. This is
	// especially important given that we're receiving
	// arbitrary commands.
	if (c->width <= x || c->height <= y || x < 0 || y < 0 ||
	    x > UINT16_MAX || y > UINT16_MAX)
		return;

	// Color space is little endian, thus the BGRA format used below:
	uint8_t mapped[4] = { color.b, color.g, color.r, 0xFF };

	size_t off = (c->stride * y) + (x * sizeof(mapped));
	memcpy(&c->buffer[off], mapped, sizeof(mapped));
}
