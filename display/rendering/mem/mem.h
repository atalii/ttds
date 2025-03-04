#pragma once

#include "../canvas.h"

#define MEM_BACKEND_WIDTH 640
#define MEM_BACKEND_HEIGHT 480

void *mem_rendering_init(void);
void mem_rendering_cleanup(void *mem_ctx);
void mem_rendering_ctx_log(const void *mem_ctx);
void mem_rendering_show(void *mem_ctx, struct canvas *);
struct canvas *mem_canvas_init(void *mem_ctx);
void *mem_input_thread(void *);
