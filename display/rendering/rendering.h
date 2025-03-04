#pragma once

#include "canvas.h"

struct rendering_vtable {
	/// Initialize the rendering backend, returning an opaque context.
	/// Returns null if allocation fails.
	void *(*rendering_init)(void);

	/// Deinitialize the rendering backend, invalidating the context.
	void (*rendering_cleanup)(void *r_ctx);

	/// Log backend-specific parameters.
	void (*rendering_ctx_log)(const void *r_ctx);

	/// Give the backend a canvas to display.
	void (*rendering_show)(void *r_ctx, struct canvas *);

	/// Construct a canvas with parameters matching the backend.
	/// Returns null if allocation fails.
	struct canvas *(*canvas_init)(void *r_ctx);

	/// This thread handles input, whatever that means for the specific
	/// backend. For implementors, the thread must call `term_block` before
	/// it returns. The return value is unused.
	void *(*input_thread)(void *unused);
};

enum backend {
	BACKEND_DRM,
	BACKEND_MEM,
};

extern const struct rendering_vtable supported_backends[];
extern const size_t backend_count;
