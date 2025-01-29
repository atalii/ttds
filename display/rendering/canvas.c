#include "canvas.h"

#include "abort.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void draw_point(struct canvas *, int32_t x, int32_t y, struct color);
static inline intmax_t min(intmax_t, intmax_t);

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

void rendering_draw_rect(
    struct canvas *c, const struct rect *rect, struct color color)
{
	uint16_t right_edge = rect->x + rect->w;
	uint16_t bottom_edge = rect->y + rect->h;
	for (uint16_t y = rect->y; y < bottom_edge && y < c->height; y++)
		for (uint16_t x = rect->x; x < right_edge && x < c->width; x++)
			draw_point(c, x, y, color);
}

void rendering_draw_circle(
    struct canvas *c, const struct circle *circle, struct color color)
{
	uint16_t x = circle->r;
	uint16_t y = 0;
	int32_t t1 = circle->r / 16;

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
}

void rendering_copy_rect(
    struct canvas *c, uint16_t dst_x, uint16_t dst_y, const struct rect *src)
{
	// The regions may overlap, so we care whether we are incrementing or
	// decrementing the y coordinate while traversing the source. When the
	// copy is solely horizontal, we prefer incrementing.
	const bool y_inc = dst_y <= src->y; // Flipped because +y is down.

	// TODO: so verbose

	const uint32_t src_right_edge = (uint32_t)src->x + src->w;
	const uint32_t dst_right_edge = (uint32_t)dst_x + src->w;
	const uint32_t src_bottom_edge = (uint32_t)src->y + src->h;
	const uint32_t dst_bottom_edge = (uint32_t)dst_y + src->h;

	const int32_t src_safe_width =
	    (int32_t)min(c->width, src_right_edge) - src->x;
	const int32_t dst_safe_width =
	    (int32_t)min(c->width, dst_right_edge) - dst_x;

	const int32_t src_safe_height =
	    (int32_t)min(c->height, src_bottom_edge) - src->y;
	const int32_t dst_safe_height =
	    (int32_t)min(c->height, dst_bottom_edge) - dst_y;

	const int32_t safe_width = min(dst_safe_width, src_safe_width);
	const int32_t safe_height = min(dst_safe_height, src_safe_height);
	if (safe_width <= 0)
		return;

	for (int32_t dy = y_inc ? 0 : safe_height - 1;
	    dy < safe_height && dy >= 0; y_inc ? dy++ : dy--) {
		uint16_t src_row_y = src->y + dy;
		uint16_t dst_row_y = dst_y + dy;
		size_t src_idx = (size_t)c->stride * src_row_y + (src->x * 4);
		size_t dst_idx = (size_t)c->stride * dst_row_y + (dst_x * 4);
		memmove(&c->buffer[dst_idx], &c->buffer[src_idx],
		    (size_t)safe_width * 4);
	}
}

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

static inline intmax_t min(intmax_t a, intmax_t b)
{
	return a < b ? a : b;
}
