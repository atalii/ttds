#pragma once

#include <dirent.h>
#include <stddef.h>
#include <stdint.h>

/* Encode a color. Alpha is assumed to always be 0xff. */
struct color {
	uint8_t r, g, b;
};

struct rect {
	uint16_t x, y, w, h;
	struct color c;
};

struct rect_copy {
	uint16_t dst_x, dst_y;
	uint16_t src_x, src_y;
	uint16_t w, h;
};

struct circle {
	uint16_t x, y, r;
	struct color c;
};

struct line {
	uint16_t x0, y0;
	uint16_t x1, y1;
	struct color c;
};

struct bezier2 {
	// Control points are wider than u16 to allow expressing
	// curves across the whole canvas.
	int32_t x0, y0;
	int32_t x1, y1;
	int32_t x2, y2;
	struct color c;
};

struct triangle {
	uint16_t x0, y0;
	uint16_t x1, y1;
	uint16_t x2, y2;
	struct color c;
};

struct canvas {
	uint16_t width, height;
	uint32_t stride;
	uint8_t *buffer;
};

struct canvas *canvas_init_bgra(uint16_t width, uint16_t height);

void canvas_deinit(struct canvas *);

void rendering_fill(struct canvas *, struct color);

#define DECL_RENDERING_FNS(type)                                          \
	void rendering_draw_##type(struct canvas *, const struct type *); \
	void rendering_draw_##type##_type_erased(struct canvas *, const void *);

DECL_RENDERING_FNS(rect)
DECL_RENDERING_FNS(circle)
DECL_RENDERING_FNS(line)
DECL_RENDERING_FNS(rect_copy)
DECL_RENDERING_FNS(bezier2)
DECL_RENDERING_FNS(triangle)

void rendering_dump_bgra_to_rgba(
    const struct canvas *c, DIR *dir, const char *dirpath, const char *path);
