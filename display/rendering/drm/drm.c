#include "../../abort.h"
#include "../canvas.h"

#include <assert.h>
#include <dirent.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef int card_fd_t;
typedef uint32_t crtc_id_t;
typedef uint32_t plane_id_t;
typedef uint32_t buf_id_t;

struct rendering_ctx;

/* A series of functions called by rendering_init that fatally error on failure,
otherwise initalaizing their respective parts of the ctx. */
static void init_card(struct rendering_ctx *);
static void init_res(struct rendering_ctx *);
static void init_conn(struct rendering_ctx *);
static void init_crtc(struct rendering_ctx *);
static void init_mode(struct rendering_ctx *);
static void init_plane(struct rendering_ctx *);
static void init_buf(struct rendering_ctx *, size_t);

static card_fd_t find_card(void);
static void require_dumb_buffers(card_fd_t);
static void require_universal_planes(card_fd_t);
static bool is_primary_plane(struct rendering_ctx *, plane_id_t plane_id);

struct buffer {
	buf_id_t id;
	uint32_t stride;
	uint64_t size;
	uint8_t *data;
};

struct rendering_ctx {
	card_fd_t card_fd;
	drmModeRes *res;
	drmModeConnector *conn;

	drmModeCrtc *crtc;

	drmModeModeInfo mode;
	drmModePlane *plane;

	size_t front_buf_idx;
	struct buffer bufs[2];
};

void *drm_rendering_init(void)
{
	struct rendering_ctx *ctx = malloc(sizeof(struct rendering_ctx));
	ctx->front_buf_idx = 0;

	init_card(ctx);
	init_res(ctx);
	init_conn(ctx);
	init_crtc(ctx);
	init_mode(ctx);
	init_plane(ctx);
	init_buf(ctx, 0);
	init_buf(ctx, 1);

	return ctx;
}

void drm_rendering_cleanup(void *r_ctx)
{
	struct rendering_ctx *ctx = r_ctx;

	int r;

	drmModeFreePlane(ctx->plane);
	drmModeFreeCrtc(ctx->crtc);
	drmModeFreeConnector(ctx->conn);
	drmModeFreeResources(ctx->res);

	r = close(ctx->card_fd);
	if (r != 0)
		FATAL_ERR("failed to close card: %s", STR_ERR);

	free(ctx);
}

void drm_rendering_ctx_log(const void *r_ctx)
{
	const struct rendering_ctx *ctx = r_ctx;

	fprintf(stderr, "CRTC:\t%d\n", ctx->crtc->crtc_id);
	fprintf(stderr, "Plane:\t%d\n", ctx->plane->plane_id);
	fprintf(stderr, "Buffer:\t%d (front), %d (back)\n",
	    ctx->bufs[ctx->front_buf_idx].id,
	    ctx->bufs[1 ^ ctx->front_buf_idx].id);
	fprintf(stderr, "Mode:\t%dx%d @ %dHz\n", ctx->mode.hdisplay,
	    ctx->mode.vdisplay, ctx->mode.vrefresh);
}

void drm_rendering_show(void *r_ctx, struct canvas *c)
{
	struct rendering_ctx *ctx = r_ctx;

	struct buffer back = ctx->bufs[1 ^ ctx->front_buf_idx];
	memcpy(back.data, c->buffer, back.size);

	int r =
	    drmModePageFlip(ctx->card_fd, ctx->crtc->crtc_id, back.id, 0, NULL);

	if (r != 0)
		FATAL_ERR("drmModePageFlip failed: %s", STR_ERR);

	// We now have a new front buffer.
	ctx->front_buf_idx ^= 1;
}

struct canvas *drm_canvas_init(void *r_ctx)
{
	struct rendering_ctx *ctx = r_ctx;

	struct buffer front = ctx->bufs[ctx->front_buf_idx];

	struct canvas *ret = malloc(sizeof(struct canvas));
	ret->width = ctx->mode.hdisplay;
	ret->height = ctx->mode.vdisplay;
	ret->stride = front.stride;
	ret->buffer = malloc(front.size);

	return ret;
}

static void init_card(struct rendering_ctx *ctx)
{
	ctx->card_fd = find_card();

	require_dumb_buffers(ctx->card_fd);
	require_universal_planes(ctx->card_fd);
}

static void init_res(struct rendering_ctx *ctx)
{
	drmModeRes *res = drmModeGetResources(ctx->card_fd);
	if (!res)
		FATAL_ERR("drmModeGetResources failed: %s", STR_ERR);

	ctx->res = res;
}

static void init_conn(struct rendering_ctx *ctx)
{
	int num_connectors = ctx->res->count_connectors;
	for (int i = 0; i < num_connectors; i++) {
		int conn_id = ctx->res->connectors[i];
		drmModeConnector *conn =
		    drmModeGetConnector(ctx->card_fd, conn_id);

		if (!conn)
			FATAL_ERR("drmModeGetConnector failed: %s", STR_ERR);

		if (conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0) {
			ctx->conn = conn;
			return;
		}

		// TODO: perhaps log any connectors we skip.

		drmModeFreeConnector(conn);
	}

	FATAL_ERR("No connectors are available.");
}

static void init_crtc(struct rendering_ctx *ctx)
{
	drmModeCrtc *crtc = NULL;

	int ncrtcs = ctx->res->count_crtcs;

	for (int i = 0; i < ncrtcs; i++) {
		crtc_id_t crtc_id = ctx->res->crtcs[i];
		crtc = drmModeGetCrtc(ctx->card_fd, crtc_id);
		if (!crtc)
			FATAL_ERR("drmModeGetCrtc failed: %s", STR_ERR);

		if (crtc->mode_valid)
			break;

		drmModeFreeCrtc(crtc);
		crtc = NULL;
	}

	if (!crtc)
		FATAL_ERR("No appropriate CRTC found.");

	ctx->crtc = crtc;
}

static void init_mode(struct rendering_ctx *ctx)
{
	ctx->mode = ctx->crtc->mode;
}

static void init_plane(struct rendering_ctx *ctx)
{
	drmModePlane *plane = NULL;
	drmModePlaneRes *planes = drmModeGetPlaneResources(ctx->card_fd);

	for (uint32_t i = 0; i < planes->count_planes; i++) {
		plane_id_t plane_id = planes->planes[i];
		plane = drmModeGetPlane(ctx->card_fd, plane_id);

		if (plane->crtc_id == ctx->crtc->crtc_id &&
		    is_primary_plane(ctx, plane_id)) {
			break;
		}

		drmModeFreePlane(plane);
		plane = NULL;
	}

	drmModeFreePlaneResources(planes);

	if (!plane)
		FATAL_ERR("No valid plane found.");

	ctx->plane = plane;
}

static void init_buf(struct rendering_ctx *ctx, size_t idx)
{
	struct buffer *target = &ctx->bufs[idx];

	uint32_t width = ctx->mode.hdisplay;
	uint32_t height = ctx->mode.vdisplay;

	struct drm_mode_create_dumb fb_create = {
		.width = width,
		.height = height,
		.bpp = 32,
	};

	if (drmIoctl(ctx->card_fd, DRM_IOCTL_MODE_CREATE_DUMB, &fb_create) != 0)
		FATAL_ERR("DRM_IOCTL_MODE_CREATE_DUMB failed: %s", STR_ERR);

	buf_id_t buf_id;
	target->stride = fb_create.pitch;
	target->size = fb_create.size;

	uint32_t handles[4] = { fb_create.handle };
	uint32_t strides[4] = { fb_create.pitch };
	uint32_t offsets[4] = { 0 };

	if (drmModeAddFB2(ctx->card_fd, width, height, DRM_FORMAT_XRGB8888,
		handles, strides, offsets, &buf_id, 0) != 0)
		FATAL_ERR("drmModeAddFB2 failed: %s", STR_ERR);

	target->id = buf_id;

	uint64_t offset;
	if (drmModeMapDumbBuffer(ctx->card_fd, fb_create.handle, &offset) != 0)
		FATAL_ERR("drmModeMapDumbBuffer failed: %s", STR_ERR);

	target->data = mmap(0, fb_create.size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, ctx->card_fd, offset);

	if (!target->data)
		FATAL_ERR("Couldn't map frame buffer.");
}

static card_fd_t find_card(void)
{
	DIR *dris = opendir("/dev/dri");

	if (!dris)
		FATAL_ERR("Failed to open /dev/dri: %s", STR_ERR);

	struct dirent *card_candidate = NULL;

	while ((card_candidate = readdir(dris))) {
		if (strncmp(card_candidate->d_name, "card", 4) == 0)
			break;
	}

	if (!card_candidate)
		FATAL_ERR("No appropriate card found.");

	card_fd_t fd =
	    openat(dirfd(dris), card_candidate->d_name, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		FATAL_ERR("Failed to open device: %s: %s",
		    card_candidate->d_name, STR_ERR);

	closedir(dris);

	return fd;
}

static void require_dumb_buffers(card_fd_t card_fd)
{
	uint64_t cap = 0;
	if (drmGetCap(card_fd, DRM_CAP_DUMB_BUFFER, &cap) < 0)
		FATAL_ERR("DRM_IOCTL_GET_CAP failed: %s", strerror(errno));

	if (!cap)
		FATAL_ERR("Device doesn't support dumb buffers.");
}

static void require_universal_planes(card_fd_t card_fd)
{
	if (drmSetClientCap(card_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
		FATAL_ERR("Could not set DRM_CLIENT_CAP_UNIVERSAL_PLANES.");
}

static bool is_primary_plane(struct rendering_ctx *ctx, plane_id_t plane_id)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(
	    ctx->card_fd, plane_id, DRM_MODE_OBJECT_PLANE);

	if (!props)
		FATAL_ERR("drmModeObjectGetProperties failed: %s", STR_ERR);

	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop =
		    drmModeGetProperty(ctx->card_fd, props->props[i]);

		if (strcmp(prop->name, "type") != 0) {
			drmModeFreeProperty(prop);
			continue;
		}

		uint64_t val = props->prop_values[i];
		drmModeFreeProperty(prop);
		drmModeFreeObjectProperties(props);
		return val == DRM_PLANE_TYPE_PRIMARY;
	}

	FATAL_ERR("No primary plane could be found.");
}
