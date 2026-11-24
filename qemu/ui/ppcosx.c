/*
 * ppcosx display: the guest's screen for the ppcOSX app, which draws it in
 * its own windows (so QEMU itself shows nothing: no window, menu bar or
 * Dock icon).
 *
 *   -display ppcosx,socket=PATH
 *
 * QEMU listens on the Unix socket PATH; the app connects (again after it
 * restarts; a new connection replaces the old one). Messages both ways are
 * a header of two little-endian uint32s, type and payload length, then the
 * payload (little-endian fields).
 *
 * To the app:
 *   HELLO     version
 *   SURFACE   width, height: a new screen size; its pixels follow as UPDATEs
 *   UPDATE    x, y, w, h, then w*h pixels, 32-bit x8r8g8b8 (BGRA in memory)
 *   FRAME     the updates of one refresh are complete: show them. Nothing
 *             more is sent until the app answers with FRAME_DONE, so a busy
 *             app gets fewer, larger updates instead of a backlog.
 *   CURSOR    width, height, hot x, hot y, then the pixels, 32-bit ARGB
 *             (BGRA in memory, not premultiplied): the guest's pointer
 *   POINTER   x, y, visible: where the guest put its pointer
 *   MOUSE     absolute: 1 when the machine has an absolute pointing device
 *             (the USB tablet), 0 when it needs relative motion
 *
 * From the app:
 *   KEY        macOS virtual key code, down
 *   MOUSE_ABS  x, y in screen pixels
 *   MOUSE_REL  dx, dy (signed)
 *   BUTTON     button (0 left, 1 middle, 2 right, 3/4 wheel up/down,
 *              5/6 wheel left/right), down
 *   FRAME_DONE the last FRAME is on screen
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"
#include <sys/socket.h>
#include <sys/un.h>

#define PPCOSX_VERSION      1

enum {
    MSG_HELLO = 1,
    MSG_SURFACE,
    MSG_UPDATE,
    MSG_FRAME,
    MSG_CURSOR,
    MSG_POINTER,
    MSG_MOUSE,
};

enum {
    IN_KEY = 1,
    IN_MOUSE_ABS,
    IN_MOUSE_REL,
    IN_BUTTON,
    IN_FRAME_DONE,
};

typedef struct PPCOSXDisplay {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    char *path;
    int listen_fd;
    int fd;                     /* the app, or -1 */

    GByteArray *out;            /* not yet written */
    size_t out_pos;
    GByteArray *in;             /* not yet a whole message */

    pixman_region32_t dirty;
    bool new_size;              /* send SURFACE before the next frame */
    bool waiting;               /* a FRAME is not yet acknowledged */

    QEMUCursor *cursor;
    int ptr_x, ptr_y;
    bool ptr_on;
    Notifier mouse_mode;

    uint32_t key_down[128 / 32];
} PPCOSXDisplay;

static PPCOSXDisplay *ppcosx_display;

/* ---- output ---- */

static void ppcosx_writable(void *opaque);
static void ppcosx_readable(void *opaque);

static void ppcosx_watch(PPCOSXDisplay *d)
{
    bool pending = d->out->len > d->out_pos;

    qemu_set_fd_handler(d->fd, ppcosx_readable,
                        pending ? ppcosx_writable : NULL, d);
}

static void ppcosx_flush(PPCOSXDisplay *d)
{
    while (d->fd >= 0 && d->out_pos < d->out->len) {
        ssize_t n = write(d->fd, d->out->data + d->out_pos,
                          d->out->len - d->out_pos);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return;             /* the read side notices the hangup */
        }
        d->out_pos += n;
    }
    if (d->out_pos == d->out->len) {
        g_byte_array_set_size(d->out, 0);
        d->out_pos = 0;
    }
    if (d->fd >= 0) {
        ppcosx_watch(d);
    }
}

static void ppcosx_writable(void *opaque)
{
    ppcosx_flush(opaque);
}

static void put32(PPCOSXDisplay *d, uint32_t v)
{
    v = cpu_to_le32(v);
    g_byte_array_append(d->out, (uint8_t *)&v, 4);
}

static void put_header(PPCOSXDisplay *d, uint32_t type, uint32_t len)
{
    put32(d, type);
    put32(d, len);
}

static void send_pixels(PPCOSXDisplay *d, int x, int y, int w, int h)
{
    pixman_image_t *dst;
    size_t off;

    put_header(d, MSG_UPDATE, 16 + w * h * 4);
    put32(d, x);
    put32(d, y);
    put32(d, w);
    put32(d, h);
    off = d->out->len;
    g_byte_array_set_size(d->out, off + w * h * 4);
    /* converts from whatever format the surface has */
    dst = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h,
                                   (uint32_t *)(d->out->data + off), w * 4);
    pixman_image_composite(PIXMAN_OP_SRC, d->surface->image, NULL, dst,
                           x, y, 0, 0, 0, 0, w, h);
    pixman_image_unref(dst);
}

static void send_cursor(PPCOSXDisplay *d)
{
    QEMUCursor *c = d->cursor;
    int i;

    if (!c) {
        return;
    }
    put_header(d, MSG_CURSOR, 16 + c->width * c->height * 4);
    put32(d, c->width);
    put32(d, c->height);
    put32(d, c->hot_x);
    put32(d, c->hot_y);
    for (i = 0; i < c->width * c->height; i++) {
        put32(d, c->data[i]);
    }
}

static void send_pointer(PPCOSXDisplay *d)
{
    put_header(d, MSG_POINTER, 12);
    put32(d, d->ptr_x);
    put32(d, d->ptr_y);
    put32(d, d->ptr_on);
}

static void send_mouse_mode(PPCOSXDisplay *d)
{
    put_header(d, MSG_MOUSE, 4);
    /* a tablet, if there is one, whether or not the guest last used the
     * relative mouse that mac99 always has beside it */
    put32(d, qemu_input_has_absolute(d->dcl.con));
}

/* the changed parts of the screen, as one frame */
static void ppcosx_send_frame(PPCOSXDisplay *d)
{
    pixman_box32_t *rects;
    int i, n;

    if (d->fd < 0 || d->waiting || !d->surface) {
        return;
    }
    if (d->new_size) {
        int w = surface_width(d->surface), h = surface_height(d->surface);

        put_header(d, MSG_SURFACE, 8);
        put32(d, w);
        put32(d, h);
        pixman_region32_fini(&d->dirty);
        pixman_region32_init_rect(&d->dirty, 0, 0, w, h);
        d->new_size = false;
    }
    if (!pixman_region32_not_empty(&d->dirty)) {
        return;
    }
    rects = pixman_region32_rectangles(&d->dirty, &n);
    for (i = 0; i < n; i++) {
        send_pixels(d, rects[i].x1, rects[i].y1,
                    rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
    }
    pixman_region32_clear(&d->dirty);
    put_header(d, MSG_FRAME, 0);
    d->waiting = true;
    ppcosx_flush(d);
}

/* ---- input ---- */

static void release_keys(PPCOSXDisplay *d)
{
    int k;

    for (k = 0; k < 128; k++) {
        if (d->key_down[k / 32] & (1u << (k % 32))) {
            qemu_input_event_send_key_qcode(d->dcl.con,
                                            qemu_input_map_osx_to_qcode[k],
                                            false);
        }
    }
    memset(d->key_down, 0, sizeof(d->key_down));
}

static void ppcosx_key(PPCOSXDisplay *d, uint32_t code, bool down)
{
    if (code >= qemu_input_map_osx_to_qcode_len || code >= 128) {
        return;
    }
    if (down) {
        d->key_down[code / 32] |= 1u << (code % 32);
    } else {
        d->key_down[code / 32] &= ~(1u << (code % 32));
    }
    qemu_input_event_send_key_qcode(d->dcl.con,
                                    qemu_input_map_osx_to_qcode[code], down);
}

static void ppcosx_button(PPCOSXDisplay *d, uint32_t b, bool down)
{
    static const InputButton map[] = {
        INPUT_BUTTON_LEFT, INPUT_BUTTON_MIDDLE, INPUT_BUTTON_RIGHT,
        INPUT_BUTTON_WHEEL_UP, INPUT_BUTTON_WHEEL_DOWN,
        INPUT_BUTTON_WHEEL_LEFT, INPUT_BUTTON_WHEEL_RIGHT,
    };

    if (b >= ARRAY_SIZE(map)) {
        return;
    }
    qemu_input_queue_btn(d->dcl.con, map[b], down);
    qemu_input_event_sync();
}

static void ppcosx_message(PPCOSXDisplay *d, uint32_t type,
                           const uint32_t *p, uint32_t len)
{
    switch (type) {
    case IN_KEY:
        if (len >= 8) {
            ppcosx_key(d, le32_to_cpu(p[0]), le32_to_cpu(p[1]));
        }
        break;
    case IN_MOUSE_ABS:
        if (len >= 8 && d->surface) {
            qemu_input_queue_abs(d->dcl.con, INPUT_AXIS_X, le32_to_cpu(p[0]),
                                 0, surface_width(d->surface));
            qemu_input_queue_abs(d->dcl.con, INPUT_AXIS_Y, le32_to_cpu(p[1]),
                                 0, surface_height(d->surface));
            qemu_input_event_sync();
        }
        break;
    case IN_MOUSE_REL:
        if (len >= 8) {
            qemu_input_queue_rel(d->dcl.con, INPUT_AXIS_X,
                                 (int32_t)le32_to_cpu(p[0]));
            qemu_input_queue_rel(d->dcl.con, INPUT_AXIS_Y,
                                 (int32_t)le32_to_cpu(p[1]));
            qemu_input_event_sync();
        }
        break;
    case IN_BUTTON:
        if (len >= 8) {
            ppcosx_button(d, le32_to_cpu(p[0]), le32_to_cpu(p[1]));
        }
        break;
    case IN_FRAME_DONE:
        d->waiting = false;
        break;
    }
}

static void ppcosx_disconnect(PPCOSXDisplay *d)
{
    if (d->fd < 0) {
        return;
    }
    qemu_set_fd_handler(d->fd, NULL, NULL, NULL);
    close(d->fd);
    d->fd = -1;
    g_byte_array_set_size(d->out, 0);
    d->out_pos = 0;
    g_byte_array_set_size(d->in, 0);
    d->waiting = false;
    release_keys(d);
}

static void ppcosx_readable(void *opaque)
{
    PPCOSXDisplay *d = opaque;
    uint8_t buf[4096];
    ssize_t n;
    size_t pos = 0;

    n = read(d->fd, buf, sizeof(buf));
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        ppcosx_disconnect(d);
        return;
    }
    if (n < 0) {
        return;
    }
    g_byte_array_append(d->in, buf, n);
    while (d->in->len - pos >= 8) {
        uint32_t type = ldl_le_p(d->in->data + pos);
        uint32_t len = ldl_le_p(d->in->data + pos + 4);
        uint32_t words[4];

        if (len > 1024) {       /* nothing the app sends is this big */
            ppcosx_disconnect(d);
            return;
        }
        if (d->in->len - pos - 8 < len) {
            break;
        }
        memset(words, 0, sizeof(words));
        memcpy(words, d->in->data + pos + 8, MIN(len, sizeof(words)));
        ppcosx_message(d, type, words, len);
        pos += 8 + len;
    }
    g_byte_array_remove_range(d->in, 0, pos);
}

static void ppcosx_accept(void *opaque)
{
    PPCOSXDisplay *d = opaque;
    int fd = qemu_accept(d->listen_fd, NULL, NULL);

    if (fd < 0) {
        return;
    }
    ppcosx_disconnect(d);       /* a newer app replaces the old one */
    qemu_socket_set_nonblock(fd);
    d->fd = fd;
    put_header(d, MSG_HELLO, 4);
    put32(d, PPCOSX_VERSION);
    d->new_size = true;
    send_mouse_mode(d);
    send_cursor(d);
    send_pointer(d);
    ppcosx_watch(d);
    ppcosx_send_frame(d);
}

/* ---- display listener ---- */

static void ppcosx_refresh(DisplayChangeListener *dcl)
{
    PPCOSXDisplay *d = container_of(dcl, PPCOSXDisplay, dcl);

    graphic_hw_update(dcl->con);
    ppcosx_send_frame(d);
}

static void ppcosx_gfx_update(DisplayChangeListener *dcl,
                              int x, int y, int w, int h)
{
    PPCOSXDisplay *d = container_of(dcl, PPCOSXDisplay, dcl);

    pixman_region32_union_rect(&d->dirty, &d->dirty, x, y, w, h);
}

static void ppcosx_gfx_switch(DisplayChangeListener *dcl,
                              DisplaySurface *surface)
{
    PPCOSXDisplay *d = container_of(dcl, PPCOSXDisplay, dcl);
    bool same = d->surface && surface &&
        surface_width(d->surface) == surface_width(surface) &&
        surface_height(d->surface) == surface_height(surface);

    d->surface = surface;
    if (same) {
        /* a new surface of the same size (cards switch often): only its
         * pixels are new */
        pixman_region32_union_rect(&d->dirty, &d->dirty, 0, 0,
                                   surface_width(surface),
                                   surface_height(surface));
    } else {
        d->new_size = true;
    }
}

static void ppcosx_mouse_set(DisplayChangeListener *dcl, int x, int y,
                             bool on)
{
    PPCOSXDisplay *d = container_of(dcl, PPCOSXDisplay, dcl);

    d->ptr_x = x;
    d->ptr_y = y;
    d->ptr_on = on;
    if (d->fd >= 0) {
        send_pointer(d);
        ppcosx_flush(d);
    }
}

static void ppcosx_cursor_define(DisplayChangeListener *dcl,
                                 QEMUCursor *cursor)
{
    PPCOSXDisplay *d = container_of(dcl, PPCOSXDisplay, dcl);

    cursor_ref(cursor);
    if (d->cursor) {
        cursor_unref(d->cursor);
    }
    d->cursor = cursor;
    if (d->fd >= 0) {
        send_cursor(d);
        ppcosx_flush(d);
    }
}

static void ppcosx_mouse_mode_change(Notifier *n, void *data)
{
    PPCOSXDisplay *d = container_of(n, PPCOSXDisplay, mouse_mode);

    if (d->fd >= 0) {
        send_mouse_mode(d);
        ppcosx_flush(d);
    }
}

static const DisplayChangeListenerOps ppcosx_ops = {
    .dpy_name = "ppcosx",
    .dpy_refresh = ppcosx_refresh,
    .dpy_gfx_update = ppcosx_gfx_update,
    .dpy_gfx_switch = ppcosx_gfx_switch,
    .dpy_mouse_set = ppcosx_mouse_set,
    .dpy_cursor_define = ppcosx_cursor_define,
};

static void ppcosx_cleanup(void)
{
    if (ppcosx_display && ppcosx_display->path) {
        unlink(ppcosx_display->path);
    }
}

static void ppcosx_display_init(DisplayState *ds, DisplayOptions *opts)
{
    PPCOSXDisplay *d = g_new0(PPCOSXDisplay, 1);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    if (!opts->u.ppcosx.socket) {
        error_report("-display ppcosx needs socket=PATH");
        exit(1);
    }
    d->path = g_strdup(opts->u.ppcosx.socket);
    if (strlen(d->path) >= sizeof(addr.sun_path)) {
        error_report("-display ppcosx: socket path too long: %s", d->path);
        exit(1);
    }
    strcpy(addr.sun_path, d->path);
    unlink(d->path);
    d->listen_fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
    if (d->listen_fd < 0 ||
        bind(d->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(d->listen_fd, 1) < 0) {
        error_report("-display ppcosx: cannot listen on %s: %s", d->path,
                     strerror(errno));
        exit(1);
    }
    qemu_socket_set_nonblock(d->listen_fd);
    qemu_set_fd_handler(d->listen_fd, ppcosx_accept, NULL, d);
    atexit(ppcosx_cleanup);

    d->fd = -1;
    d->out = g_byte_array_new();
    d->in = g_byte_array_new();
    pixman_region32_init(&d->dirty);
    d->mouse_mode.notify = ppcosx_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&d->mouse_mode);

    d->dcl.ops = &ppcosx_ops;
    d->dcl.con = qemu_console_lookup_default();
    register_displaychangelistener(&d->dcl);
    /* about 60 frames a second */
    update_displaychangelistener(&d->dcl, 16);
    ppcosx_display = d;
}

static QemuDisplay qemu_display_ppcosx = {
    .type = DISPLAY_TYPE_PPCOSX,
    .init = ppcosx_display_init,
};

static void register_ppcosx(void)
{
    qemu_display_register(&qemu_display_ppcosx);
}

type_init(register_ppcosx);
