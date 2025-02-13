#include "rendering.h"

#include "drm/drm.h"
#include "mem/mem.h"

const struct rendering_vtable
    supported_backends[] = {
	[BACKEND_DRM] = {
		.rendering_init = drm_rendering_init,
		.rendering_cleanup = drm_rendering_cleanup,
		.rendering_ctx_log = drm_rendering_ctx_log,
		.rendering_show = drm_rendering_show,
		.canvas_init = drm_canvas_init,
		.input_thread = drm_input_thread,
	},
	[BACKEND_MEM] = {
		.rendering_init = mem_rendering_init,
		.rendering_cleanup = mem_rendering_cleanup,
		.rendering_ctx_log = mem_rendering_ctx_log,
		.rendering_show = mem_rendering_show,
		.canvas_init = mem_canvas_init,
		.input_thread = mem_input_thread,
	}
};

const size_t backend_count =
    sizeof(supported_backends) / sizeof(*supported_backends);
