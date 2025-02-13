#pragma once

#include "../canvas.h"
#include "input.h"

void *drm_rendering_init(void);
void drm_rendering_cleanup(void *drm_ctx);
void drm_rendering_ctx_log(const void *drm_ctx);
void drm_rendering_show(void *drm_ctx, struct canvas *);
struct canvas *drm_canvas_init(void *drm_ctx);
