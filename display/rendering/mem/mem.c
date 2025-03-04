#include "mem.h"

#include "threads/termination.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *mem_rendering_init(void)
{
	// TODO: hardcoded resolution is unfortunate, perhaps pass optional size
	// hint ignored by DRM backend?
	return canvas_init_bgra(MEM_BACKEND_WIDTH, MEM_BACKEND_HEIGHT);
}

void mem_rendering_cleanup(void *mem_ctx)
{
	struct canvas *ctx = mem_ctx;
	canvas_deinit(ctx);
}

void mem_rendering_ctx_log(const void *mem_ctx)
{
	const struct canvas *ctx = mem_ctx;

	fprintf(stderr, "Size:\t%dx%d\n", ctx->width, ctx->height);
}

void mem_rendering_show(void *mem_ctx, struct canvas *c)
{
	struct canvas *ctx = mem_ctx;
	assert(ctx->width == c->width && ctx->height == c->height &&
	    ctx->stride == c->stride);

	size_t buffer_size = (size_t)ctx->stride * ctx->height;
	memcpy(ctx->buffer, c->buffer, buffer_size);
}

struct canvas *mem_canvas_init(void *mem_ctx)
{
	struct canvas *ctx = mem_ctx;
	return canvas_init_bgra(ctx->width, ctx->height);
}

void *mem_input_thread(void *)
{
	term_block();
	return NULL;
}
