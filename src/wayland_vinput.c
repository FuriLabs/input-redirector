/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#define _GNU_SOURCE

#include "wayland_vinput.h"

#include <sys/mman.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_seat *seat;
    uint32_t seat_caps;
    char seat_name[128];

    struct wl_output *output;
    int out_width;
    int out_height;
    int32_t out_transform;

    struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;

    struct zwp_virtual_keyboard_v1 *vkbd;
    struct zwlr_virtual_pointer_v1 *vptr;

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;

    gboolean initialized;
    gchar *wayland_display;

    GMutex mutex;
    gboolean mutex_initialized;
} WaylandVInput;

static WaylandVInput g_wl = {0};

static uint32_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    uint64_t ms = (uint64_t) ts.tv_sec * 1000ULL +
                  (uint64_t) ts.tv_nsec / 1000000ULL;

    return (uint32_t) ms;
}

static void
wayland_cleanup(void)
{
    g_debug("wayland_vinput: cleanup");

    if (g_wl.vptr) {
        g_debug("wayland_vinput: destroy vptr");
        zwlr_virtual_pointer_v1_destroy(g_wl.vptr);
        g_wl.vptr = NULL;
    }

    if (g_wl.vkbd) {
        g_debug("wayland_vinput: destroy vkbd");
        zwp_virtual_keyboard_v1_destroy(g_wl.vkbd);
        g_wl.vkbd = NULL;
    }

    if (g_wl.vptr_mgr) {
        g_debug("wayland_vinput: destroy vptr_mgr");
        zwlr_virtual_pointer_manager_v1_destroy(g_wl.vptr_mgr);
        g_wl.vptr_mgr = NULL;
    }

    if (g_wl.vkbd_mgr) {
        g_debug("wayland_vinput: destroy vkbd_mgr");
        zwp_virtual_keyboard_manager_v1_destroy(g_wl.vkbd_mgr);
        g_wl.vkbd_mgr = NULL;
    }

    if (g_wl.output) {
        g_debug("wayland_vinput: destroy output");
        wl_output_destroy(g_wl.output);
        g_wl.output = NULL;
    }

    if (g_wl.seat) {
        g_debug("wayland_vinput: destroy seat");
        wl_seat_destroy(g_wl.seat);
        g_wl.seat = NULL;
    }

    if (g_wl.registry) {
        g_debug("wayland_vinput: destroy registry");
        wl_registry_destroy(g_wl.registry);
        g_wl.registry = NULL;
    }

    if (g_wl.display) {
        g_debug("wayland_vinput: disconnect display");
        wl_display_disconnect(g_wl.display);
        g_wl.display = NULL;
    }

    if (g_wl.xkb_keymap) {
        g_debug("wayland_vinput: unref xkb_keymap");
        xkb_keymap_unref(g_wl.xkb_keymap);
        g_wl.xkb_keymap = NULL;
    }

    if (g_wl.xkb_ctx) {
        g_debug("wayland_vinput: unref xkb_ctx");
        xkb_context_unref(g_wl.xkb_ctx);
        g_wl.xkb_ctx = NULL;
    }

    g_wl.initialized = FALSE;
    g_wl.out_width = 0;
    g_wl.out_height = 0;
    g_wl.out_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    g_wl.seat_caps = 0;
    g_wl.seat_name[0] = '\0';
}

static void
wayland_handle_disconnect(const char *where)
{
    int werr = 0;

    if (g_wl.display)
        werr = wl_display_get_error(g_wl.display);

    g_debug("wayland_vinput: Wayland connection error at %s (wl_error=%d, errno=%d: %s). reconnecting",
            where ? where : "unknown",
            werr,
            errno,
            g_strerror(errno));

    wayland_cleanup();
}

static void
pump(void)
{
    if (!g_wl.display)
        return;

    if (wl_display_flush(g_wl.display) < 0) {
        g_debug("wayland_vinput: wl_display_flush failed");
        wayland_handle_disconnect("wl_display_flush");
        return;
    }

    if (wl_display_dispatch_pending(g_wl.display) < 0) {
        g_debug("wayland_vinput: wl_display_dispatch_pending failed");
        wayland_handle_disconnect("wl_display_dispatch_pending");
        return;
    }

    if (g_wl.display && wl_display_get_error(g_wl.display) != 0) {
        g_debug("wayland_vinput: wl_display_get_error indicates failure");
        wayland_handle_disconnect("wl_display_get_error");
        return;
    }
}

static int
create_memfd(size_t size)
{
    int fd = memfd_create("vkbd-keymap", MFD_CLOEXEC);

    if (fd < 0) {
        g_debug("wayland_vinput: memfd_create failed");
        return -1;
    }

    if (ftruncate(fd, (off_t) size) < 0) {
        g_debug("wayland_vinput: ftruncate failed");
        close(fd);
        return -1;
    }

    return fd;
}

static gboolean
send_default_keymap(void)
{
    if (!g_wl.vkbd) {
        g_debug("wayland_vinput: send_default_keymap called without vkbd");
        return FALSE;
    }

    if (!g_wl.xkb_ctx) {
        g_wl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!g_wl.xkb_ctx) {
            g_debug("wayland_vinput: xkb_context_new failed");
            return FALSE;
        }
        g_debug("wayland_vinput: created xkb_context");
    }

    if (!g_wl.xkb_keymap) {
        g_wl.xkb_keymap = xkb_keymap_new_from_names(
            g_wl.xkb_ctx,
            NULL,
            XKB_KEYMAP_COMPILE_NO_FLAGS
        );

        if (!g_wl.xkb_keymap) {
            g_debug("wayland_vinput: xkb_keymap_new_from_names failed");
            return FALSE;
        }
        g_debug("wayland_vinput: created xkb_keymap");
    }

    char *keymap_str = xkb_keymap_get_as_string(g_wl.xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);

    if (!keymap_str) {
        g_debug("wayland_vinput: xkb_keymap_get_as_string failed");
        return FALSE;
    }

    size_t size = strlen(keymap_str) + 1;

    int fd = create_memfd(size);
    if (fd < 0) {
        free(keymap_str);
        return FALSE;
    }

    ssize_t wr = write(fd, keymap_str, size);
    if (wr < 0 || (size_t) wr != size) {
        g_debug("wayland_vinput: write(keymap) failed");
        close(fd);
        free(keymap_str);
        return FALSE;
    }

    lseek(fd, 0, SEEK_SET);

    g_debug("wayland_vinput: sending keymap (size=%zu)", size);

    zwp_virtual_keyboard_v1_keymap(
        g_wl.vkbd,
        WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
        fd,
        (uint32_t) size
    );

    close(fd);
    free(keymap_str);

    pump();
    return TRUE;
}

static void
wl_seat_capabilities(void *data,
                     struct wl_seat *seat,
                     uint32_t caps)
{
    (void) data;
    (void) seat;

    g_wl.seat_caps = caps;
    g_debug("wayland_vinput: wl_seat.capabilities = 0x%x", caps);
}

static void
wl_seat_name(void *data,
             struct wl_seat *seat,
             const char *name)
{
    (void) data;
    (void) seat;

    g_strlcpy(g_wl.seat_name, name ? name : "", sizeof(g_wl.seat_name));
    g_debug("wayland_vinput: wl_seat.name = %s", g_wl.seat_name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

static void
wl_output_geometry(void *data,
                   struct wl_output *output,
                   int32_t x,
                   int32_t y,
                   int32_t phys_width,
                   int32_t phys_height,
                   int32_t subpixel,
                   const char *make,
                   const char *model,
                   int32_t transform)
{
    (void) data;
    (void) output;
    (void) x;
    (void) y;
    (void) phys_width;
    (void) phys_height;
    (void) subpixel;
    (void) make;
    (void) model;

    g_wl.out_transform = transform;

    g_debug("wayland_vinput: wl_output transform = %d", transform);
}

static void
wl_output_mode(void *data,
               struct wl_output *output,
               uint32_t flags,
               int32_t width,
               int32_t height,
               int32_t refresh)
{
    (void) data;
    (void) output;
    (void) refresh;

    if (flags & WL_OUTPUT_MODE_CURRENT) {
        g_wl.out_width = width;
        g_wl.out_height = height;
        g_debug("wayland_vinput: wl_output current mode %dx%d", width, height);
    } else if (g_wl.out_width <= 0 || g_wl.out_height <= 0) {
        g_wl.out_width = width;
        g_wl.out_height = height;
        g_debug("wayland_vinput: wl_output mode %dx%d", width, height);
    }
}

static void
wl_output_done(void *data,
               struct wl_output *output)
{
    (void) data;
    (void) output;
}

static void
wl_output_scale(void *data,
                struct wl_output *output,
                int32_t factor)
{
    (void) data;
    (void) output;
    (void) factor;
}

static const struct wl_output_listener output_listener = {
    .geometry = wl_output_geometry,
    .mode = wl_output_mode,
    .done = wl_output_done,
    .scale = wl_output_scale,
};

static void
registry_global(void *data,
                struct wl_registry *registry,
                uint32_t name,
                const char *interface,
                uint32_t version)
{
    (void) data;

    if (strcmp(interface, "wl_seat") == 0) {
        if (!g_wl.seat) {
            uint32_t v = version > 9 ? 9 : version;

            g_wl.seat = wl_registry_bind(
                registry,
                name,
                &wl_seat_interface,
                v
            );

            wl_seat_add_listener(g_wl.seat, &seat_listener, NULL);
            g_debug("wayland_vinput: bound wl_seat (global=%u, version=%u)", name, v);
        }
    } else if (strcmp(interface, "wl_output") == 0) {
        if (!g_wl.output) {
            uint32_t v = version > 2 ? 2 : version;

            g_wl.output = wl_registry_bind(
                registry,
                name,
                &wl_output_interface,
                v
            );

            wl_output_add_listener(g_wl.output, &output_listener, NULL);
            g_debug("wayland_vinput: bound wl_output (global=%u, version=%u)", name, v);
        }
    } else if (strcmp(interface, "zwp_virtual_keyboard_manager_v1") == 0) {
        uint32_t v = version > 1 ? 1 : version;

        g_wl.vkbd_mgr = wl_registry_bind(
            registry,
            name,
            &zwp_virtual_keyboard_manager_v1_interface,
            v
        );

        g_debug("wayland_vinput: bound zwp_virtual_keyboard_manager_v1 (global=%u, version=%u)", name, v);
    } else if (strcmp(interface, "zwlr_virtual_pointer_manager_v1") == 0) {
        uint32_t v = version > 2 ? 2 : version;

        g_wl.vptr_mgr = wl_registry_bind(
            registry,
            name,
            &zwlr_virtual_pointer_manager_v1_interface,
            v
        );

        g_debug("wayland_vinput: bound zwlr_virtual_pointer_manager_v1 (global=%u, version=%u)", name, v);
    }
}

static void
registry_global_remove(void *data,
                       struct wl_registry *registry,
                       uint32_t name)
{
    (void) data;
    (void) registry;
    (void) name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void
ensure_initialized(void)
{
    if (g_wl.display && wl_display_get_error(g_wl.display) != 0)
        wayland_handle_disconnect("ensure_initialized(precheck)");

    if (g_wl.initialized)
        return;

    if (g_wl.wayland_display && *g_wl.wayland_display) {
        g_debug("wayland_vinput: setting WAYLAND_DISPLAY=%s", g_wl.wayland_display);
        g_setenv("WAYLAND_DISPLAY", g_wl.wayland_display, TRUE);
    }

    g_wl.display = wl_display_connect(NULL);
    if (!g_wl.display) {
        g_debug("wayland_vinput: wl_display_connect failed");
        return;
    }

    g_wl.registry = wl_display_get_registry(g_wl.display);
    if (!g_wl.registry) {
        g_debug("wayland_vinput: wl_display_get_registry failed");
        wayland_handle_disconnect("wl_display_get_registry");
        return;
    }

    wl_registry_add_listener(g_wl.registry, &registry_listener, NULL);

    if (wl_display_roundtrip(g_wl.display) < 0) {
        g_debug("wayland_vinput: wl_display_roundtrip(globals) failed");
        wayland_handle_disconnect("wl_display_roundtrip(globals)");
        return;
    }

    if (wl_display_roundtrip(g_wl.display) < 0) {
        g_debug("wayland_vinput: wl_display_roundtrip(seat/output) failed");
        wayland_handle_disconnect("wl_display_roundtrip(seat/output)");
        return;
    }

    if (!g_wl.seat) {
        g_debug("wayland_vinput: no wl_seat advertised");
        wayland_handle_disconnect("no wl_seat");
        return;
    }

    if (!g_wl.vkbd_mgr) {
        g_debug("wayland_vinput: zwp_virtual_keyboard_manager_v1 not advertised");
        wayland_handle_disconnect("no vkbd_mgr");
        return;
    }

    if (!g_wl.vptr_mgr) {
        g_debug("wayland_vinput: zwlr_virtual_pointer_manager_v1 not advertised");
        wayland_handle_disconnect("no vptr_mgr");
        return;
    }

    g_wl.vkbd =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g_wl.vkbd_mgr,
                                                               g_wl.seat);
    if (!g_wl.vkbd) {
        g_debug("wayland_vinput: failed to create zwp_virtual_keyboard_v1");
        wayland_handle_disconnect("create_virtual_keyboard");
        return;
    }

    g_wl.vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(g_wl.vptr_mgr,
                                                                       g_wl.seat);
    if (!g_wl.vptr) {
        g_debug("wayland_vinput: failed to create zwlr_virtual_pointer_v1");
        wayland_handle_disconnect("create_virtual_pointer");
        return;
    }

    if (!send_default_keymap())
        g_debug("wayland_vinput: failed to send default keymap");

    if (g_wl.display && wl_display_roundtrip(g_wl.display) < 0) {
        g_debug("wayland_vinput: wl_display_roundtrip(after keymap) failed");
        wayland_handle_disconnect("wl_display_roundtrip(after keymap)");
        return;
    }

    g_wl.initialized = TRUE;

    g_debug("wayland_vinput: ready (screen=%dx%d transform=%d)",
            g_wl.out_width, g_wl.out_height, g_wl.out_transform);
}

static enum wl_pointer_axis_source
scroll_source_for_code(int code)
{
    switch (code) {
    case REL_WHEEL:
    case REL_HWHEEL:
        return WL_POINTER_AXIS_SOURCE_WHEEL;
    default:
        return WL_POINTER_AXIS_SOURCE_WHEEL;
    }
}

static double
scroll_value_for_code(int code,
                      int value)
{
    double scale;

    switch (code) {
    case REL_WHEEL:
    case REL_HWHEEL:
        scale = 1.0;
        break;
    default:
        scale = 1.0;
        break;
    }

    return (double) -value * scale;
}

void
wayland_vinput_init(const gchar *wayland_display)
{
    if (!g_wl.mutex_initialized) {
        g_mutex_init(&g_wl.mutex);
        g_wl.mutex_initialized = TRUE;
    }

    g_mutex_lock(&g_wl.mutex);

    g_free(g_wl.wayland_display);
    g_wl.wayland_display = NULL;

    if (wayland_display && *wayland_display)
        g_wl.wayland_display = g_strdup(wayland_display);

    ensure_initialized();

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_cleanup(void)
{
    if (!g_wl.mutex_initialized)
        return;

    g_mutex_lock(&g_wl.mutex);

    wayland_cleanup();

    g_free(g_wl.wayland_display);
    g_wl.wayland_display = NULL;

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_key_event(int code,
                         int value,
                         const char *thread_name)
{
    g_mutex_lock(&g_wl.mutex);

    g_debug("[%s] wayland_vinput: key_event code=%d value=%d",
            thread_name, code, value);

    ensure_initialized();

    if (!g_wl.initialized || !g_wl.vkbd) {
        g_debug("[%s] wayland_vinput: key_event called but backend not ready",
                thread_name);
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    zwp_virtual_keyboard_v1_key(
        g_wl.vkbd,
        now_ms(),
        (uint32_t) code,
        value ? WL_KEYBOARD_KEY_STATE_PRESSED
              : WL_KEYBOARD_KEY_STATE_RELEASED
    );

    pump();

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_mouse_button(int code,
                            int value,
                            const char *thread_name)
{
    g_mutex_lock(&g_wl.mutex);

    g_debug("[%s] wayland_vinput: mouse_button code=%d value=%d",
            thread_name, code, value);

    ensure_initialized();

    if (!g_wl.initialized || !g_wl.vptr) {
        g_debug("[%s] wayland_vinput: mouse_button called but backend not ready",
                thread_name);
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    zwlr_virtual_pointer_v1_button(
        g_wl.vptr,
        now_ms(),
        (uint32_t) code,
        value ? WL_POINTER_BUTTON_STATE_PRESSED
              : WL_POINTER_BUTTON_STATE_RELEASED
    );

    zwlr_virtual_pointer_v1_frame(g_wl.vptr);

    pump();

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_mouse_motion(int rel_x,
                            int rel_y,
                            const char *thread_name)
{
    g_mutex_lock(&g_wl.mutex);

    g_debug("[%s] wayland_vinput: mouse_motion dx=%d dy=%d",
            thread_name, rel_x, rel_y);

    ensure_initialized();

    if (!g_wl.initialized || !g_wl.vptr) {
        g_debug("[%s] wayland_vinput: mouse_motion called but backend not ready",
                thread_name);
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    zwlr_virtual_pointer_v1_motion(
        g_wl.vptr,
        now_ms(),
        wl_fixed_from_double((double) rel_x),
        wl_fixed_from_double((double) rel_y)
    );

    zwlr_virtual_pointer_v1_frame(g_wl.vptr);

    pump();

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_scroll(int code,
                      int value,
                      const char *thread_name)
{
    g_mutex_lock(&g_wl.mutex);

    g_debug("[%s] wayland_vinput: scroll code=%d value=%d",
            thread_name, code, value);

    ensure_initialized();

    if (!g_wl.initialized || !g_wl.vptr) {
        g_debug("[%s] wayland_vinput: scroll called but backend not ready",
                thread_name);
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    enum wl_pointer_axis axis;
    enum wl_pointer_axis_source source;
    uint32_t t;
    double amount;

    if (code == REL_WHEEL) {
        axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
    } else if (code == REL_HWHEEL) {
        axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    } else {
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    source = scroll_source_for_code(code);
    t = now_ms();
    amount = scroll_value_for_code(code, value);

    g_debug("[%s] wayland_vinput: scroll axis=%d source=%d amount=%f",
            thread_name, axis, source, amount);

    zwlr_virtual_pointer_v1_axis_source(
        g_wl.vptr,
        source
    );

    zwlr_virtual_pointer_v1_axis(
        g_wl.vptr,
        t,
        axis,
        wl_fixed_from_double(amount)
    );

    zwlr_virtual_pointer_v1_frame(g_wl.vptr);

    zwlr_virtual_pointer_v1_axis_stop(
        g_wl.vptr,
        t,
        axis
    );

    zwlr_virtual_pointer_v1_frame(g_wl.vptr);

    pump();

    g_mutex_unlock(&g_wl.mutex);
}

static void
pointer_abs(int x, int y)
{
    uint32_t raw_w = 1000;
    uint32_t raw_h = 1000;
    uint32_t extent_w;
    uint32_t extent_h;
    int tx = x;
    int ty = y;

    if (g_wl.out_width > 0)
        raw_w = (uint32_t) g_wl.out_width;

    if (g_wl.out_height > 0)
        raw_h = (uint32_t) g_wl.out_height;

    switch (g_wl.out_transform) {
    case WL_OUTPUT_TRANSFORM_90:
        extent_w = raw_h;
        extent_h = raw_w;
        tx = y;
        ty = (int) raw_w - x;
        break;

    case WL_OUTPUT_TRANSFORM_180:
        extent_w = raw_w;
        extent_h = raw_h;
        tx = (int) raw_w - x;
        ty = (int) raw_h - y;
        break;

    case WL_OUTPUT_TRANSFORM_270:
        extent_w = raw_h;
        extent_h = raw_w;
        tx = (int) raw_h - y;
        ty = x;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED:
        extent_w = raw_w;
        extent_h = raw_h;
        tx = (int) raw_w - x;
        ty = y;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        extent_w = raw_h;
        extent_h = raw_w;
        tx = y;
        ty = x;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        extent_w = raw_w;
        extent_h = raw_h;
        tx = x;
        ty = (int) raw_h - y;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        extent_w = raw_h;
        extent_h = raw_w;
        tx = (int) raw_h - y;
        ty = (int) raw_w - x;
        break;

    case WL_OUTPUT_TRANSFORM_NORMAL:
    default:
        extent_w = raw_w;
        extent_h = raw_h;
        break;
    }

    if (tx < 0)
        tx = 0;
    if (ty < 0)
        ty = 0;

    if ((uint32_t) tx > extent_w)
        tx = (int) extent_w;
    if ((uint32_t) ty > extent_h)
        ty = (int) extent_h;

    g_debug("wayland_vinput: pointer_abs raw=(%d,%d) transformed=(%d,%d) extent=%ux%u transform=%d",
            x, y, tx, ty, extent_w, extent_h, g_wl.out_transform);

    zwlr_virtual_pointer_v1_motion_absolute(
        g_wl.vptr,
        now_ms(),
        (uint32_t) tx,
        (uint32_t) ty,
        extent_w,
        extent_h
    );

    zwlr_virtual_pointer_v1_frame(g_wl.vptr);
}

void
wayland_vinput_touch_down(int x,
                          int y,
                          const char *thread_name)
{
    g_mutex_lock(&g_wl.mutex);

    g_debug("[%s] wayland_vinput: touch_down x=%d y=%d",
            thread_name, x, y);

    ensure_initialized();

    if (!g_wl.initialized || !g_wl.vptr) {
        g_debug("[%s] wayland_vinput: touch_down called but backend not ready",
                thread_name);
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    pointer_abs(x, y);

    zwlr_virtual_pointer_v1_button(
        g_wl.vptr,
        now_ms(),
        BTN_LEFT,
        WL_POINTER_BUTTON_STATE_PRESSED
    );

    zwlr_virtual_pointer_v1_frame(g_wl.vptr);

    pump();

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_touch_move(int x,
                          int y,
                          const char *thread_name)
{
    g_mutex_lock(&g_wl.mutex);

    g_debug("[%s] wayland_vinput: touch_move x=%d y=%d",
            thread_name, x, y);

    ensure_initialized();

    if (!g_wl.initialized || !g_wl.vptr) {
        g_debug("[%s] wayland_vinput: touch_move called but backend not ready",
                thread_name);
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    pointer_abs(x, y);

    pump();

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_touch_up(const char *thread_name)
{
    g_mutex_lock(&g_wl.mutex);

    g_debug("[%s] wayland_vinput: touch_up",
            thread_name);

    ensure_initialized();

    if (!g_wl.initialized || !g_wl.vptr) {
        g_debug("[%s] wayland_vinput: touch_up called but backend not ready",
                thread_name);
        g_mutex_unlock(&g_wl.mutex);
        return;
    }

    zwlr_virtual_pointer_v1_button(
        g_wl.vptr,
        now_ms(),
        BTN_LEFT,
        WL_POINTER_BUTTON_STATE_RELEASED
    );

    zwlr_virtual_pointer_v1_frame(g_wl.vptr);

    pump();

    g_mutex_unlock(&g_wl.mutex);
}

void
wayland_vinput_get_screen_size(unsigned int *width,
                               unsigned int *height)
{
    g_mutex_lock(&g_wl.mutex);

    if (width) {
        if (g_wl.out_width > 0)
            *width = (unsigned int) g_wl.out_width;
        else
            *width = 0;
    }

    if (height) {
        if (g_wl.out_height > 0)
            *height = (unsigned int) g_wl.out_height;
        else
            *height = 0;
    }

    g_mutex_unlock(&g_wl.mutex);
}
