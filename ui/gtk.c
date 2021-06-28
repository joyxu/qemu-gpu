/*
 * GTK UI
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Portions from gtk-vnc (originally licensed under the LGPL v2+):
 *
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
 */

#define GETTEXT_PACKAGE "qemu"
#define LOCALEDIR "po"

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/cutils.h"

#include "ui/console.h"
#include "ui/gtk.h"
#ifdef G_OS_WIN32
#include <gdk/gdkwin32.h>
#endif
#include "ui/win32-kbd-hook.h"

#include <glib/gi18n.h>
#include <locale.h>
#if defined(CONFIG_VTE)
#include <vte/vte.h>
#endif
#include <math.h>

#include "trace.h"
#include "qemu/cutils.h"
#include "ui/input.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"
#include "keymaps.h"
#include "chardev/char.h"
#include "qom/object.h"

#define VC_WINDOW_X_MIN  320
#define VC_WINDOW_Y_MIN  240
#define VC_TERM_X_MIN     80
#define VC_TERM_Y_MIN     25
#define VC_SCALE_MIN    0.25
#define VC_SCALE_STEP   0.25

#ifdef GDK_WINDOWING_X11
#include "x_keymap.h"

/* Gtk2 compat */
#ifndef GDK_IS_X11_DISPLAY
#define GDK_IS_X11_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_WAYLAND
/* Gtk2 compat */
#ifndef GDK_IS_WAYLAND_DISPLAY
#define GDK_IS_WAYLAND_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_WIN32
/* Gtk2 compat */
#ifndef GDK_IS_WIN32_DISPLAY
#define GDK_IS_WIN32_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_BROADWAY
/* Gtk2 compat */
#ifndef GDK_IS_BROADWAY_DISPLAY
#define GDK_IS_BROADWAY_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_QUARTZ
/* Gtk2 compat */
#ifndef GDK_IS_QUARTZ_DISPLAY
#define GDK_IS_QUARTZ_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#if !defined(CONFIG_VTE)
# define VTE_CHECK_VERSION(a, b, c) 0
#endif

#define HOTKEY_MODIFIERS        (GDK_CONTROL_MASK | GDK_ALT_MASK)

static const guint16 *keycode_map;
static size_t keycode_maplen;

struct VCChardev {
    Chardev parent;
    VirtualConsole *console;
    bool echo;
};
typedef struct VCChardev VCChardev;

#define TYPE_CHARDEV_VC "chardev-vc"
DECLARE_INSTANCE_CHECKER(VCChardev, VC_CHARDEV,
                         TYPE_CHARDEV_VC)

bool gtk_use_gl_area;

static void gd_grab_pointer(VirtualConsole *vc, const char *reason);
static void gd_ungrab_pointer(GtkDisplayState *s);
static void gd_grab_keyboard(VirtualConsole *vc, const char *reason);
static void gd_ungrab_keyboard(GtkDisplayState *s);

/** Utility Functions **/

static VirtualConsole *gd_vc_find_by_menu(GtkDisplayState *s)
{
    VirtualConsole *vc;
    gint i;

    for (i = 0; i < s->nb_vcs; i++) {
        vc = &s->vc[i];
        if (gtk_check_button_get_active
            (GTK_CHECK_BUTTON(vc->menu_item))) {
            return vc;
        }
    }
    return NULL;
}

static VirtualConsole *gd_vc_find_by_page(GtkDisplayState *s, gint page)
{
    VirtualConsole *vc;
    gint i, p;

    for (i = 0; i < s->nb_vcs; i++) {
        vc = &s->vc[i];
        p = gtk_notebook_page_num(GTK_NOTEBOOK(s->notebook), vc->tab_item);
        if (p == page) {
            return vc;
        }
    }
    return NULL;
}

static VirtualConsole *gd_vc_find_current(GtkDisplayState *s)
{
    gint page;

    page = gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook));
    return gd_vc_find_by_page(s, page);
}

static bool gd_is_grab_active(GtkDisplayState *s)
{
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(s->grab_item));
}

static bool gd_grab_on_hover(GtkDisplayState *s)
{
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(s->grab_on_hover_item));
}

static void gd_update_cursor(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    GdkSurface *surface;
    GtkNative *native;

    if (vc->type != GD_VC_GFX ||
        !qemu_console_is_graphic(vc->gfx.dcl.con)) {
        return;
    }

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    native = gtk_widget_get_native(GTK_WIDGET(vc->gfx.drawing_area));
    surface = gtk_native_get_surface(native);
    if (s->full_screen || qemu_input_is_absolute() || s->ptr_owner == vc) {
        gdk_surface_set_cursor(surface, s->null_cursor);
    } else {
        gdk_surface_set_cursor(surface, NULL);
    }
}

static void gd_update_caption(GtkDisplayState *s)
{
    const char *status = "";
    gchar *prefix;
    gchar *title;
    const char *grab = "";
    bool is_paused = !runstate_is_running();
    int i;

    if (qemu_name) {
        prefix = g_strdup_printf("QEMU (%s)", qemu_name);
    } else {
        prefix = g_strdup_printf("QEMU");
    }

    if (s->ptr_owner != NULL &&
        s->ptr_owner->surface == NULL) {
        grab = _(" - Press Ctrl+Alt+G to release grab");
    }

    if (is_paused) {
        status = _(" [Paused]");
    }

    title = g_strdup_printf("%s%s%s", prefix, status, grab);
    gtk_window_set_title(GTK_WINDOW(s->window), title);
    g_free(title);

    for (i = 0; i < s->nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];

        if (!vc->window) {
            continue;
        }
        title = g_strdup_printf("%s: %s%s%s", prefix, vc->label,
                                vc == s->kbd_owner ? " +kbd" : "",
                                vc == s->ptr_owner ? " +ptr" : "");
        gtk_window_set_title(GTK_WINDOW(vc->window), title);
        g_free(title);
    }

    g_free(prefix);
}

static void gd_update_geometry_hints(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    int min_width, min_height;
    GtkWidget *geo_widget = NULL;
    GtkWindow *geo_window;

    if (vc->type == GD_VC_GFX) {
        if (!vc->gfx.ds) {
            return;
        }
        if (s->free_scale) {
            min_width  = surface_width(vc->gfx.ds) * VC_SCALE_MIN;
            min_height = surface_height(vc->gfx.ds) * VC_SCALE_MIN;
            // TODO no hints?
           // mask |= GDK_HINT_MIN_SIZE;
        } else {
            min_width  = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
            min_height = surface_height(vc->gfx.ds) * vc->gfx.scale_y;
          //  mask |= GDK_HINT_MIN_SIZE;
        }
        geo_widget = vc->gfx.drawing_area;
        gtk_widget_set_size_request(geo_widget, min_width, min_height);

#if defined(CONFIG_VTE)
    } else if (vc->type == GD_VC_VTE) {
        VteTerminal *term = VTE_TERMINAL(vc->vte.terminal);
        GtkBorder padding = { 0 };

#if VTE_CHECK_VERSION(0, 37, 0)
        gtk_style_context_get_padding(
                gtk_widget_get_style_context(vc->vte.terminal),
                gtk_widget_get_state_flags(vc->vte.terminal),
                &padding);
#else
        {
            GtkBorder *ib = NULL;
            gtk_widget_style_get(vc->vte.terminal, "inner-border", &ib, NULL);
            if (ib) {
                padding = *ib;
                gtk_border_free(ib);
            }
        }
#endif

        geo.width_inc  = vte_terminal_get_char_width(term);
        geo.height_inc = vte_terminal_get_char_height(term);
        mask |= GDK_HINT_RESIZE_INC;
        geo.base_width  = geo.width_inc;
        geo.base_height = geo.height_inc;
        mask |= GDK_HINT_BASE_SIZE;
        geo.min_width  = geo.width_inc * VC_TERM_X_MIN;
        geo.min_height = geo.height_inc * VC_TERM_Y_MIN;
        mask |= GDK_HINT_MIN_SIZE;

        geo.base_width  += padding.left + padding.right;
        geo.base_height += padding.top + padding.bottom;
        geo.min_width   += padding.left + padding.right;
        geo.min_height  += padding.top + padding.bottom;
        geo_widget = vc->vte.terminal;
#endif
    }

    geo_window = GTK_WINDOW(vc->window ? vc->window : s->window);
    gtk_window_set_default_size(geo_window, min_width, min_height);
}

void gd_update_windowsize(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;

    gd_update_geometry_hints(vc);

    if (vc->type == GD_VC_GFX && !s->full_screen && !s->free_scale) {
        gtk_window_set_default_size(GTK_WINDOW(vc->window ? vc->window : s->window),
                          VC_WINDOW_X_MIN, VC_WINDOW_Y_MIN);
    }
}

static void gd_update_full_redraw(VirtualConsole *vc)
{
#if defined(CONFIG_OPENGL)
    if (vc->gfx.gls && gtk_use_gl_area) {
        gtk_gl_area_queue_render(GTK_GL_AREA(vc->gfx.drawing_area));
        return;
    }
#endif
    gtk_widget_queue_draw(vc->gfx.drawing_area);
}

static void gtk_release_modifiers(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (vc->type != GD_VC_GFX ||
        !qemu_console_is_graphic(vc->gfx.dcl.con)) {
        return;
    }
    qkbd_state_lift_all_keys(vc->gfx.kbd);
}

static void gd_widget_reparent(GtkWidget *from, GtkWidget *to,
                               GtkWidget *widget)
{
    gtk_widget_set_parent(widget, to);
}

static void *gd_win32_get_hwnd(VirtualConsole *vc)
{
#ifdef G_OS_WIN32
    return gdk_win32_window_get_impl_hwnd(
        gtk_widget_get_window(vc->window ? vc->window : vc->s->window));
#else
    return NULL;
#endif
}

/** DisplayState Callbacks **/

static void gd_update(DisplayChangeListener *dcl,
                      int x, int y, int w, int h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GtkNative *native;
    GdkSurface *surface;
    int x1, x2, y1, y2;
    int mx, my;
    int fbw, fbh;
    int sw, sh;

    trace_gd_update(vc->label, x, y, w, h);

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    if (vc->gfx.convert) {
        pixman_image_composite(PIXMAN_OP_SRC, vc->gfx.ds->image,
                               NULL, vc->gfx.convert,
                               x, y, 0, 0, x, y, w, h);
    }

    /*
    x1 = floor(x * vc->gfx.scale_x);
    y1 = floor(y * vc->gfx.scale_y);

    x2 = ceil(x * vc->gfx.scale_x + w * vc->gfx.scale_x);
    y2 = ceil(y * vc->gfx.scale_y + h * vc->gfx.scale_y);

    fbw = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    fbh = surface_height(vc->gfx.ds) * vc->gfx.scale_y;

    native = gtk_widget_get_native(vc->gfx.drawing_area);
    surface = gtk_native_get_surface(native);
    if (!surface) {
        return;
    }
    sw = gdk_surface_get_width(surface);
    sh = gdk_surface_get_height(surface);

    mx = my = 0;
    if (sw > fbw) {
        mx = (sw - fbw) / 2;
    }
    if (sh > fbh) {
        my = (sh - fbh) / 2;
    }

*/
    // TODO: equivalent in GTK4?
    gtk_widget_queue_draw(vc->gfx.drawing_area); // gtk_widget_queue_draw_area(vc->gfx.drawing_area, mx + x1, my + y1, (x2 - x1), (y2 - y1));
}

static void gd_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static GdkDevice *gd_get_pointer(GdkDisplay *dpy)
{
    return gdk_seat_get_pointer(gdk_display_get_default_seat(dpy));
}

static void gd_mouse_set(DisplayChangeListener *dcl,
                         int x, int y, int visible)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    //GdkDisplay *dpy;
    //gint x_root, y_root;

    if (qemu_input_is_absolute()) {
        return;
    }


    /*
    dpy = gtk_widget_get_display(vc->gfx.drawing_area);
    gdk_window_get_root_coords(gtk_widget_get_window(vc->gfx.drawing_area),
                               x, y, &x_root, &y_root);
    gdk_device_warp(gd_get_pointer(dpy),
                    gtk_widget_get_screen(vc->gfx.drawing_area),
                    x_root, y_root);
                    */
    vc->s->last_x = x;
    vc->s->last_y = y;
}

static void gd_cursor_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkPixbuf *pixbuf;
    GdkCursor *cursor;

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    pixbuf = gdk_pixbuf_new_from_data((guchar *)(c->data),
                                      GDK_COLORSPACE_RGB, true, 8,
                                      c->width, c->height, c->width * 4,
                                      NULL, NULL);
    GdkTexture *texture = gdk_texture_new_for_pixbuf(pixbuf);
    cursor = gdk_cursor_new_from_texture(texture, c->hot_x, c->hot_y, NULL);
    gdk_surface_set_cursor(gtk_native_get_surface(gtk_widget_get_native(vc->gfx.drawing_area)), cursor);
    g_object_unref(pixbuf);
    g_object_unref(texture);
    g_object_unref(cursor);
}

static void gd_switch(DisplayChangeListener *dcl,
                      DisplaySurface *surface)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.surface) {
        cairo_surface_destroy(vc->gfx.surface);
        vc->gfx.surface = NULL;
    }
    if (vc->gfx.convert) {
        pixman_image_unref(vc->gfx.convert);
        vc->gfx.convert = NULL;
    }

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }
    vc->gfx.ds = surface;

    if (surface->format == PIXMAN_x8r8g8b8) {
        /*
         * PIXMAN_x8r8g8b8 == CAIRO_FORMAT_RGB24
         *
         * No need to convert, use surface directly.  Should be the
         * common case as this is qemu_default_pixelformat(32) too.
         */
        vc->gfx.surface = cairo_image_surface_create_for_data
            (surface_data(surface),
             CAIRO_FORMAT_RGB24,
             surface_width(surface),
             surface_height(surface),
             surface_stride(surface));
    } else {
        /* Must convert surface, use pixman to do it. */
        vc->gfx.convert = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                                   surface_width(surface),
                                                   surface_height(surface),
                                                   NULL, 0);
        vc->gfx.surface = cairo_image_surface_create_for_data
            ((void *)pixman_image_get_data(vc->gfx.convert),
             CAIRO_FORMAT_RGB24,
             pixman_image_get_width(vc->gfx.convert),
             pixman_image_get_height(vc->gfx.convert),
             pixman_image_get_stride(vc->gfx.convert));
        pixman_image_composite(PIXMAN_OP_SRC, vc->gfx.ds->image,
                               NULL, vc->gfx.convert,
                               0, 0, 0, 0, 0, 0,
                               pixman_image_get_width(vc->gfx.convert),
                               pixman_image_get_height(vc->gfx.convert));
    }

    if (resized) {
        gd_update_windowsize(vc);
    } else {
        gd_update_full_redraw(vc);
    }
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name             = "gtk",
    .dpy_gfx_update       = gd_update,
    .dpy_gfx_switch       = gd_switch,
    .dpy_gfx_check_format = qemu_pixman_check_format,
    .dpy_refresh          = gd_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,
};


#if defined(CONFIG_OPENGL)

static bool gd_has_dmabuf(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (gtk_use_gl_area && !gtk_widget_get_realized(vc->gfx.drawing_area)) {
        /* FIXME: Assume it will work, actual check done after realize */
        /* fixing this would require delaying listener registration */
        return true;
    }

    return vc->gfx.has_dmabuf;
}

/** DisplayState Callbacks (opengl version) **/

static const DisplayChangeListenerOps dcl_gl_area_ops = {
    .dpy_name             = "gtk-egl",
    .dpy_gfx_update       = gd_gl_area_update,
    .dpy_gfx_switch       = gd_gl_area_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_refresh          = gd_gl_area_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,

    .dpy_gl_ctx_create       = gd_gl_area_create_context,
    .dpy_gl_ctx_destroy      = gd_gl_area_destroy_context,
    .dpy_gl_ctx_make_current = gd_gl_area_make_current,
    .dpy_gl_scanout_texture  = gd_gl_area_scanout_texture,
    .dpy_gl_scanout_disable  = gd_gl_area_scanout_disable,
    .dpy_gl_update           = gd_gl_area_scanout_flush,
    .dpy_gl_scanout_dmabuf   = gd_gl_area_scanout_dmabuf,
    .dpy_has_dmabuf          = gd_has_dmabuf,
};

#ifdef CONFIG_X11

static const DisplayChangeListenerOps dcl_egl_ops = {
    .dpy_name             = "gtk-egl",
    .dpy_gfx_update       = gd_egl_update,
    .dpy_gfx_switch       = gd_egl_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_refresh          = gd_egl_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,

    .dpy_gl_ctx_create       = gd_egl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = gd_egl_make_current,
    .dpy_gl_scanout_disable  = gd_egl_scanout_disable,
    .dpy_gl_scanout_texture  = gd_egl_scanout_texture,
    .dpy_gl_scanout_dmabuf   = gd_egl_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = gd_egl_cursor_dmabuf,
    .dpy_gl_cursor_position  = gd_egl_cursor_position,
    .dpy_gl_release_dmabuf   = gd_egl_release_dmabuf,
    .dpy_gl_update           = gd_egl_scanout_flush,
    .dpy_has_dmabuf          = gd_has_dmabuf,
};

#endif

#endif /* CONFIG_OPENGL */

/** QEMU Events **/

static void gd_change_runstate(void *opaque, bool running, RunState state)
{
    GtkDisplayState *s = opaque;

    gd_update_caption(s);
}

static void gd_mouse_mode_change(Notifier *notify, void *data)
{
    GtkDisplayState *s;
    int i;

    s = container_of(notify, GtkDisplayState, mouse_mode_notifier);
    /* release the grab at switching to absolute mode */
    if (qemu_input_is_absolute() && gd_is_grab_active(s)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(s->grab_item),
                                       FALSE);
    }
    for (i = 0; i < s->nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];
        gd_update_cursor(vc);
    }
}

/** GTK Events **/

static gboolean gd_window_close(GtkWidget *widget,
                                void *opaque)
{
    GtkDisplayState *s = opaque;
    bool allow_close = true;

    if (s->opts->has_window_close && !s->opts->window_close) {
        allow_close = false;
    }

    if (allow_close) {
        qmp_quit(NULL);
    }

    return TRUE;
}

static void gd_set_ui_info(VirtualConsole *vc, int width, int height)
{
    QemuUIInfo info;

    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    dpy_set_ui_info(vc->gfx.dcl.con, &info);
}

#if defined(CONFIG_OPENGL)

static gboolean gd_render_event(GtkGLArea *area, GdkGLContext *context,
                                void *opaque)
{
    VirtualConsole *vc = opaque;

    if (vc->gfx.gls) {
        gd_gl_area_draw(vc);
    }
    return TRUE;
}

static void gd_resize_event(GtkWidget *area, int width, int height, gpointer *opaque)
{
    VirtualConsole *vc = (void *)opaque;

    gd_set_ui_info(vc, width, height);
}

#endif

/*
 * If available, return the update interval of the monitor in ms,
 * else return 0 (the default update interval).
 */
int gd_monitor_update_interval(GtkWidget *widget)
{
#ifdef GDK_VERSION_3_22
    GdkWindow *win = gtk_widget_get_window(widget);

    if (win) {
        GdkDisplay *dpy = gtk_widget_get_display(widget);
        GdkMonitor *monitor = gdk_display_get_monitor_at_window(dpy, win);
        int refresh_rate = gdk_monitor_get_refresh_rate(monitor); /* [mHz] */

        if (refresh_rate) {
            /* T = 1 / f = 1 [s*Hz] / f = 1000*1000 [ms*mHz] / f */
            return MIN(1000 * 1000 / refresh_rate,
                       GUI_REFRESH_INTERVAL_DEFAULT);
        }
    }
#endif
    return 0;
}

static gboolean gd_draw_event(GtkDrawingArea *widget, cairo_t *cr, int width, int height, void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    int mx, my;
    int ww, wh;
    int fbw, fbh;

#if defined(CONFIG_OPENGL)
    if (vc->gfx.gls) {
        if (gtk_use_gl_area) {
            /* invoke render callback please */
            return FALSE;
        } else {
#ifdef CONFIG_X11
            gd_egl_draw(vc);
            return TRUE;
#else
            abort();
#endif
        }
    }
#endif

    if (!gtk_widget_get_realized(widget)) {
        return FALSE;
    }
    if (!vc->gfx.ds) {
        return FALSE;
    }

    vc->gfx.dcl.update_interval =
        gd_monitor_update_interval(vc->window ? vc->window : s->window);

    fbw = surface_width(vc->gfx.ds);
    fbh = surface_height(vc->gfx.ds);

    ww = gdk_surface_get_width(gtk_native_get_surface(gtk_widget_get_native(widget)));
    wh = gdk_surface_get_height(gtk_native_get_surface(gtk_widget_get_native(widget)));

    if (s->full_screen) {
        vc->gfx.scale_x = (double)ww / fbw;
        vc->gfx.scale_y = (double)wh / fbh;
    } else if (s->free_scale) {
        double sx, sy;

        sx = (double)ww / fbw;
        sy = (double)wh / fbh;

        vc->gfx.scale_x = vc->gfx.scale_y = MIN(sx, sy);
    }

    fbw *= vc->gfx.scale_x;
    fbh *= vc->gfx.scale_y;

    mx = my = 0;
    if (ww > fbw) {
        mx = (ww - fbw) / 2;
    }
    if (wh > fbh) {
        my = (wh - fbh) / 2;
    }

    cairo_rectangle(cr, 0, 0, ww, wh);

    /* Optionally cut out the inner area where the pixmap
       will be drawn. This avoids 'flashing' since we're
       not double-buffering. Note we're using the undocumented
       behaviour of drawing the rectangle from right to left
       to cut out the whole */
    cairo_rectangle(cr, mx + fbw, my,
                    -1 * fbw, fbh);
    cairo_fill(cr);

    cairo_scale(cr, vc->gfx.scale_x, vc->gfx.scale_y);
    cairo_set_source_surface(cr, vc->gfx.surface,
                             mx / vc->gfx.scale_x, my / vc->gfx.scale_y);
    cairo_paint(cr);

    return TRUE;
}

static gboolean gd_motion_event(GtkWidget *widget, GdkEvent *motion,
                                void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    int x, y;
    int mx, my;
    int fbh, fbw;
    int ww, wh;
    double motion_x, motion_y;

    if (!vc->gfx.ds) {
        return TRUE;
    }

    fbw = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    fbh = surface_height(vc->gfx.ds) * vc->gfx.scale_y;

    ww = gdk_surface_get_width(gtk_native_get_surface(gtk_widget_get_native(vc->gfx.drawing_area)));
    wh = gdk_surface_get_height(gtk_native_get_surface(gtk_widget_get_native(vc->gfx.drawing_area)));

    mx = my = 0;
    if (ww > fbw) {
        mx = (ww - fbw) / 2;
    }
    if (wh > fbh) {
        my = (wh - fbh) / 2;
    }

    gdk_event_get_position(motion, &motion_x, &motion_y);
    
    x = (motion_x - mx) / vc->gfx.scale_x;
    y = (motion_y - my) / vc->gfx.scale_y;

    if (qemu_input_is_absolute()) {
        if (x < 0 || y < 0 ||
            x >= surface_width(vc->gfx.ds) ||
            y >= surface_height(vc->gfx.ds)) {
            return TRUE;
        }
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_X, x,
                             0, surface_width(vc->gfx.ds));
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_Y, y,
                             0, surface_height(vc->gfx.ds));
        qemu_input_event_sync();
    } else if (s->last_set && s->ptr_owner == vc) {
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_X, x - s->last_x);
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_Y, y - s->last_y);
        qemu_input_event_sync();
    }
    s->last_x = x;
    s->last_y = y;
    s->last_set = TRUE;

    if (!qemu_input_is_absolute() && s->ptr_owner == vc) {
        GdkDisplay *dpy = gtk_widget_get_display(widget);
        GtkNative *native = gtk_widget_get_native(widget);
        GdkSurface *surface = gtk_native_get_surface(native);
        GdkMonitor *monitor = gdk_display_get_monitor_at_surface(dpy, surface);
        GdkRectangle geometry;
        int screen_width, screen_height;

        // TODO how do we get root positions?
        //int x = (int)motion->x_root;
        //int y = (int)motion->y_root;

        gdk_monitor_get_geometry(monitor, &geometry);
        screen_width = geometry.width;
        screen_height = geometry.height;

        /* In relative mode check to see if client pointer hit
         * one of the screen edges, and if so move it back by
         * 200 pixels. This is important because the pointer
         * in the server doesn't correspond 1-for-1, and so
         * may still be only half way across the screen. Without
         * this warp, the server pointer would thus appear to hit
         * an invisible wall */
        if (x == 0) {
            x += 200;
        }
        if (y == 0) {
            y += 200;
        }
        if (x == (screen_width - 1)) {
            x -= 200;
        }
        if (y == (screen_height - 1)) {
            y -= 200;
        }

        // TODO figure out root positions
        //if (x != (int)motion->x_root || y != (int)motion->y_root) {
        //    GdkDevice *dev = gdk_event_get_device((GdkEvent *)motion);
        //    gdk_device_warp(dev, screen, x, y);
        //    s->last_set = FALSE;
        //    return FALSE;
        //}
    }
    return TRUE;
}

static gboolean gd_button_event(GtkWidget *widget, GdkEvent *button,
                                void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    InputButton btn;
    guint button_num;
    GType event_type;

    button_num = gdk_button_event_get_button(button);
    event_type = gdk_event_get_event_type(button);
    /* implicitly grab the input at the first click in the relative mode */
    if (button_num == 1 && event_type == GDK_BUTTON_PRESS &&
        !qemu_input_is_absolute() && s->ptr_owner != vc) {
        if (!vc->surface) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(s->grab_item),
                                           TRUE);
        } else {
            gd_grab_pointer(vc, "relative-mode-click");
        }
        return TRUE;
    }

    if (button_num == 1) {
        btn = INPUT_BUTTON_LEFT;
    } else if (button_num == 2) {
        btn = INPUT_BUTTON_MIDDLE;
    } else if (button_num == 3) {
        btn = INPUT_BUTTON_RIGHT;
    } else if (button_num == 8) {
        btn = INPUT_BUTTON_SIDE;
    } else if (button_num == 9) {
        btn = INPUT_BUTTON_EXTRA;
    } else {
        return TRUE;
    }

    qemu_input_queue_btn(vc->gfx.dcl.con, btn,
                         event_type == GDK_BUTTON_PRESS);
    qemu_input_event_sync();
    return TRUE;
}

static gboolean gd_scroll_event(GtkWidget *widget, GdkEvent *scroll,
                                void *opaque)
{
    VirtualConsole *vc = opaque;
    InputButton btn;
    GdkScrollDirection direction;

    direction = gdk_scroll_event_get_direction(scroll);

    if (direction == GDK_SCROLL_UP) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (direction == GDK_SCROLL_DOWN) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else if (direction == GDK_SCROLL_SMOOTH) {
        double delta_x, delta_y;
        gdk_scroll_event_get_deltas(scroll, &delta_x, &delta_y);
        if (delta_y == 0) {
            return TRUE;
        } else if (delta_y > 0) {
            btn = INPUT_BUTTON_WHEEL_DOWN;
        } else {
            btn = INPUT_BUTTON_WHEEL_UP;
        }
    } else {
        return TRUE;
    }

    qemu_input_queue_btn(vc->gfx.dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(vc->gfx.dcl.con, btn, false);
    qemu_input_event_sync();
    return TRUE;
}


static const guint16 *gd_get_keymap(size_t *maplen)
{
    GdkDisplay *dpy = gdk_display_get_default();

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("x11");
        return qemu_xkeymap_mapping_table(
            gdk_x11_display_get_xdisplay(dpy), maplen);
    }
#endif

#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("wayland");
        *maplen = qemu_input_map_xorgevdev_to_qcode_len;
        return qemu_input_map_xorgevdev_to_qcode;
    }
#endif

#ifdef GDK_WINDOWING_WIN32
    if (GDK_IS_WIN32_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("win32");
        *maplen = qemu_input_map_atset1_to_qcode_len;
        return qemu_input_map_atset1_to_qcode;
    }
#endif

#ifdef GDK_WINDOWING_QUARTZ
    if (GDK_IS_QUARTZ_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("quartz");
        *maplen = qemu_input_map_osx_to_qcode_len;
        return qemu_input_map_osx_to_qcode;
    }
#endif

#ifdef GDK_WINDOWING_BROADWAY
    if (GDK_IS_BROADWAY_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("broadway");
        g_warning("experimental: using broadway, x11 virtual keysym\n"
                  "mapping - with very limited support. See also\n"
                  "https://bugzilla.gnome.org/show_bug.cgi?id=700105");
        *maplen = qemu_input_map_x11_to_qcode_len;
        return qemu_input_map_x11_to_qcode;
    }
#endif

    g_warning("Unsupported GDK Windowing platform.\n"
              "Disabling extended keycode tables.\n"
              "Please report to qemu-devel@nongnu.org\n"
              "including the following information:\n"
              "\n"
              "  - Operating system\n"
              "  - GDK Windowing system build\n");
    return NULL;
}


static int gd_map_keycode(int scancode)
{
    if (!keycode_map) {
        return 0;
    }
    if (scancode > keycode_maplen) {
        return 0;
    }

    return keycode_map[scancode];
}

static int gd_get_keycode(GdkEvent *key)
{
#ifdef G_OS_WIN32
    int scancode = gdk_event_get_scancode((GdkEvent *)key);

    /* translate Windows native scancodes to atset1 keycodes */
    switch (scancode & (KF_EXTENDED | 0xff)) {
    case 0x145:     /* NUMLOCK */
        return scancode & 0xff;
    }

    return scancode & KF_EXTENDED ?
        0xe000 | (scancode & 0xff) : scancode & 0xff;

#else
    return gdk_key_event_get_keycode(key);
#endif
}

static gboolean gd_text_key_down(GtkWidget *widget,
                                 GdkEvent *key, void *opaque)
{
    VirtualConsole *vc = opaque;
    QemuConsole *con = vc->gfx.dcl.con;
    guint keyval = gdk_key_event_get_keyval(key);

    if (keyval == GDK_KEY_Delete) {
        kbd_put_qcode_console(con, Q_KEY_CODE_DELETE, false);
    } else {
        const char* name = gdk_keyval_name(keyval);
        size_t len = strlen(name);
        if (len) {
            kbd_put_string_console(con, name, len);
        } else {
            int qcode = gd_map_keycode(gd_get_keycode(key));
            kbd_put_qcode_console(con, qcode, false);
        }
    }
    return TRUE;
}

static gboolean gd_key_event(GtkWidget *widget, GdkEvent *key, void *opaque)
{
    VirtualConsole *vc = opaque;
    int keycode, qcode;
    guint keyval = gdk_key_event_get_keyval(key);
    GdkEventType type = gdk_event_get_event_type(key);

#ifdef G_OS_WIN32
    /* on windows, we ought to ignore the reserved key event? */
    if (key->hardware_keycode == 0xff)
        return false;

    if (!vc->s->kbd_owner) {
        if (key->hardware_keycode == VK_LWIN ||
            key->hardware_keycode == VK_RWIN) {
            return FALSE;
        }
    }
#endif

    if (keyval == GDK_KEY_Pause
#ifdef G_OS_WIN32
        /* for some reason GDK does not fill keyval for VK_PAUSE
         * See https://bugzilla.gnome.org/show_bug.cgi?id=769214
         */
        || key->hardware_keycode == VK_PAUSE
#endif
        ) {
        qkbd_state_key_event(vc->gfx.kbd, Q_KEY_CODE_PAUSE,
                             type == GDK_KEY_PRESS);
        return TRUE;
    }

    keycode = gd_get_keycode(key);
    qcode = gd_map_keycode(keycode);

    trace_gd_key_event(vc->label, keycode, qcode,
                       (type == GDK_KEY_PRESS) ? "down" : "up");

    qkbd_state_key_event(vc->gfx.kbd, qcode,
                         type == GDK_KEY_PRESS);

    return TRUE;
}

static gboolean gd_grab_broken_event(GtkWidget *widget,
                                     GdkEvent *event, void *opaque)
{
#ifdef CONFIG_WIN32
    /*
     * On Windows the Ctrl-Alt-Del key combination can't be grabbed. This
     * key combination leaves all three keys in a stuck condition. We use
     * the grab-broken-event to release all keys.
     */
    if (event->keyboard) {
        VirtualConsole *vc = opaque;
        GtkDisplayState *s = vc->s;

        gtk_release_modifiers(s);
    }
#endif
    return TRUE;
}

static gboolean gd_event(GtkWidget *widget, GdkEvent *event, void *opaque)
{
    if (gdk_event_get_event_type(event) == GDK_MOTION_NOTIFY) {
        return gd_motion_event(widget, event, opaque);
    }
    return FALSE;
}

/** Window Menu Actions **/

static void gd_menu_pause(GAction *action, GVariant *args, gpointer opaque)
{
    GtkDisplayState *s = opaque;

    if (s->external_pause_update) {
        return;
    }
    if (runstate_is_running()) {
        qmp_stop(NULL);
        g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(TRUE));
    } else {
        qmp_cont(NULL);
        g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(FALSE));
    }
}

static void gd_menu_reset(GAction *item, GVariant *args, gpointer opaque)
{
    qmp_system_reset(NULL);
}

static void gd_menu_powerdown(GAction *item, GVariant *args, gpointer opaque)
{
    qmp_system_powerdown(NULL);
}

static void gd_menu_quit(GAction *item, GVariant *args, gpointer opaque)
{
    qmp_quit(NULL);
}

static void gd_menu_switch_vc(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_by_menu(s);
    GtkNotebook *nb = GTK_NOTEBOOK(s->notebook);
    gint page;

    gtk_release_modifiers(s);
    if (vc) {
        page = gtk_notebook_page_num(nb, vc->tab_item);
        gtk_notebook_set_current_page(nb, page);
        gtk_widget_grab_focus(vc->focus);
    }
}

static void gd_accel_switch_vc(GtkWidget* widget, GVariant* args, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(vc->menu_item), TRUE);
}

static void gd_menu_show_tabs(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(s->show_tabs_item))) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), TRUE);
    } else {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
    }
    gd_update_windowsize(vc);
}

static gboolean gd_tab_window_close(GtkWidget *widget, void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    gtk_widget_set_sensitive(vc->menu_item, true);
    gd_widget_reparent(vc->window, s->notebook, vc->tab_item);
    gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(s->notebook),
                                    vc->tab_item, vc->label);
    gtk_window_destroy(GTK_WINDOW(vc->window));
    vc->window = NULL;
    return TRUE;
}

static gboolean gd_win_grab(GtkWidget* widget, GVariant* args, gpointer opaque)
{
    VirtualConsole *vc = opaque;

    fprintf(stderr, "%s: %s\n", __func__, vc->label);
    if (vc->s->ptr_owner) {
        gd_ungrab_pointer(vc->s);
    } else {
        gd_grab_pointer(vc, "user-request-detached-tab");
    }
    return TRUE;
}

static void gd_menu_untabify(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (vc->type == GD_VC_GFX &&
        qemu_console_is_graphic(vc->gfx.dcl.con)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(s->grab_item),
                                       FALSE);
    }
    if (!vc->window) {
        gtk_widget_set_sensitive(vc->menu_item, false);
        GdkDisplay* display = gtk_widget_get_display(vc->window);
        vc->window = gtk_window_new();
        vc->surface = (GtkWidget*)gdk_surface_new_toplevel(display);
        gd_widget_reparent(s->notebook, vc->window, vc->tab_item);

        g_signal_connect(vc->window, "destroy",
                         G_CALLBACK(gd_tab_window_close), vc);
        gtk_widget_show(vc->window);

        if (qemu_console_is_graphic(vc->gfx.dcl.con)) {
            GtkEventController* sc = gtk_shortcut_controller_new();
            gtk_widget_add_controller(vc->window, sc);

            GtkShortcutTrigger *trigger = gtk_keyval_trigger_new(GDK_KEY_g, HOTKEY_MODIFIERS);
            GtkShortcutFunc callback = (GtkShortcutFunc)gd_win_grab;
            GtkShortcutAction *action = gtk_callback_action_new(callback, vc, NULL);
            GtkShortcut *shortcut = gtk_shortcut_new(trigger, action);
            gtk_shortcut_controller_add_shortcut((GtkShortcutController *)sc, shortcut);
        }

        gd_update_geometry_hints(vc);
        gd_update_caption(s);
    }
}

static void gd_menu_show_menubar(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (s->full_screen) {
        return;
    }

    if (gtk_check_button_get_active(
                GTK_CHECK_BUTTON(s->show_menubar_item))) {
        gtk_widget_show(s->menu_bar);
    } else {
        gtk_widget_hide(s->menu_bar);
    }
    gd_update_windowsize(vc);
}

static void gd_accel_show_menubar(void *opaque)
{
    // TODO how do we activate?
    GtkDisplayState *s = opaque;
    //gtk_button_activate(GTK_BUTTON(s->show_menubar_item));
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(s->window), true);
}

static void gd_menu_full_screen(GAction *item, GVariant *args, gpointer opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (!s->full_screen) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
        gtk_widget_hide(s->menu_bar);
        if (vc->type == GD_VC_GFX) {
            gtk_widget_set_size_request(vc->gfx.drawing_area, -1, -1);
        }
        gtk_window_fullscreen(GTK_WINDOW(s->window));
        s->full_screen = TRUE;
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(s->window));
        gd_menu_show_tabs(GTK_BUTTON(s->show_tabs_item), s);
        if (gtk_check_button_get_active(
                    GTK_CHECK_BUTTON(s->show_menubar_item))) {
            gtk_widget_show(s->menu_bar);
        }
        s->full_screen = FALSE;
        if (vc->type == GD_VC_GFX) {
            vc->gfx.scale_x = 1.0;
            vc->gfx.scale_y = 1.0;
            gd_update_windowsize(vc);
        }
    }

    gd_update_cursor(vc);
}

static void gd_accel_full_screen(GtkWidget* widget, GVariant* args, gpointer opaque)
{
    // TODO how do we activate?
    GtkDisplayState *s = opaque;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->full_screen_item), true);
}

static void gd_menu_zoom_in(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->zoom_fit_item),
                                   FALSE);

    vc->gfx.scale_x += VC_SCALE_STEP;
    vc->gfx.scale_y += VC_SCALE_STEP;

    gd_update_windowsize(vc);
}

static void gd_accel_zoom_in(void *opaque)
{
    // TODO how do I activate
    GtkDisplayState *s = opaque;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->zoom_in_item), true);
}

static void gd_menu_zoom_out(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->zoom_fit_item),
                                   FALSE);

    vc->gfx.scale_x -= VC_SCALE_STEP;
    vc->gfx.scale_y -= VC_SCALE_STEP;

    vc->gfx.scale_x = MAX(vc->gfx.scale_x, VC_SCALE_MIN);
    vc->gfx.scale_y = MAX(vc->gfx.scale_y, VC_SCALE_MIN);

    gd_update_windowsize(vc);
}

static void gd_menu_zoom_fixed(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    vc->gfx.scale_x = 1.0;
    vc->gfx.scale_y = 1.0;

    gd_update_windowsize(vc);
}

static void gd_menu_zoom_fit(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(s->zoom_fit_item))) {
        s->free_scale = TRUE;
    } else {
        s->free_scale = FALSE;
        vc->gfx.scale_x = 1.0;
        vc->gfx.scale_y = 1.0;
    }

    gd_update_windowsize(vc);
    gd_update_full_redraw(vc);
}

static void gd_grab_update(VirtualConsole *vc, bool kbd, bool ptr)
{
    /*
    // TODO do not need these anymore?
    GdkDisplay *display = gtk_widget_get_display(vc->gfx.drawing_area);
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GtkNative* native = gtk_widget_get_native(vc->gfx.drawing_area);
    GdkSurface *surface = gtk_native_get_surface(native);
    GdkSeatCapabilities caps = 0;
    GdkCursor *cursor = NULL;

    if (kbd) {
        caps |= GDK_SEAT_CAPABILITY_KEYBOARD;
    }
    if (ptr) {
        caps |= GDK_SEAT_CAPABILITY_ALL_POINTING;
        cursor = vc->s->null_cursor;
    }

    if (caps) {
        //gdk_seat_grab(seat, surface, caps, false, cursor,
            //          NULL, NULL, NULL);
    } else {
        //gdk_seat_ungrab(seat);
    }
    */
}

static void gd_grab_keyboard(VirtualConsole *vc, const char *reason)
{
    if (vc->s->kbd_owner) {
        if (vc->s->kbd_owner == vc) {
            return;
        } else {
            gd_ungrab_keyboard(vc->s);
        }
    }

    win32_kbd_set_grab(true);
    gd_grab_update(vc, true, vc->s->ptr_owner == vc);
    vc->s->kbd_owner = vc;
    gd_update_caption(vc->s);
    trace_gd_grab(vc->label, "kbd", reason);
}

static void gd_ungrab_keyboard(GtkDisplayState *s)
{
    VirtualConsole *vc = s->kbd_owner;

    if (vc == NULL) {
        return;
    }
    s->kbd_owner = NULL;

    win32_kbd_set_grab(false);
    gd_grab_update(vc, false, vc->s->ptr_owner == vc);
    gd_update_caption(s);
    trace_gd_ungrab(vc->label, "kbd");
}

static void gd_grab_pointer(VirtualConsole *vc, const char *reason)
{
    GdkDisplay *display = gtk_widget_get_display(vc->gfx.drawing_area);
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(vc->gfx.drawing_area));

    if (vc->s->ptr_owner) {
        if (vc->s->ptr_owner == vc) {
            return;
        } else {
            gd_ungrab_pointer(vc->s);
        }
    }

    gd_grab_update(vc, vc->s->kbd_owner == vc, true);
    // TODO equivalent?
    double x, y;
    gdk_surface_get_device_position(surface, gd_get_pointer(display), &x, &y, NULL); // <- gdk_device_get_position(gd_get_pointer(display),
    //                        NULL, &vc->s->grab_x_root, &vc->s->grab_y_root);
    vc->s->grab_x_root = (int)x;
    vc->s->grab_y_root = (int)y;
    vc->s->ptr_owner = vc;
    gd_update_caption(vc->s);
    trace_gd_grab(vc->label, "ptr", reason);
}

static void gd_ungrab_pointer(GtkDisplayState *s)
{
    VirtualConsole *vc = s->ptr_owner;
    //GdkDisplay *display;

    if (vc == NULL) {
        return;
    }
    s->ptr_owner = NULL;

   //display = gtk_widget_get_display(vc->gfx.drawing_area);
    gd_grab_update(vc, vc->s->kbd_owner == vc, false);
    // TODO warp?
   // gdk_device_warp(gd_get_pointer(display),
   //                 gtk_widget_get_screen(vc->gfx.drawing_area),
   //                 vc->s->grab_x_root, vc->s->grab_y_root);
    gd_update_caption(s);
    trace_gd_ungrab(vc->label, "ptr");
}

static void gd_menu_grab_input(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (gd_is_grab_active(s)) {
        gd_grab_keyboard(vc, "user-request-main-window");
        gd_grab_pointer(vc, "user-request-main-window");
    } else {
        gd_ungrab_keyboard(s);
        gd_ungrab_pointer(s);
    }

    gd_update_cursor(vc);
}

static void gd_change_page(GtkNotebook *nb, gpointer arg1, guint arg2,
                           gpointer data)
{
    GtkDisplayState *s = data;
    VirtualConsole *vc;
    gboolean on_vga;

    if (!gtk_widget_get_realized(s->notebook)) {
        return;
    }

    vc = gd_vc_find_by_page(s, arg2);
    if (!vc) {
        return;
    }
    gtk_check_button_set_active(GTK_CHECK_BUTTON(vc->menu_item),
                                   TRUE);
    on_vga = (vc->type == GD_VC_GFX &&
              qemu_console_is_graphic(vc->gfx.dcl.con));
    if (!on_vga) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(s->grab_item),
                                       FALSE);
    } else if (s->full_screen) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(s->grab_item),
                                       TRUE);
    }
    gtk_widget_set_sensitive(s->grab_item, on_vga);
#ifdef CONFIG_VTE
    gtk_widget_set_sensitive(s->copy_item, vc->type == GD_VC_VTE);
#endif

    gd_update_windowsize(vc);
    gd_update_cursor(vc);
}

static gboolean gd_enter_event(GtkWidget *widget, GdkEvent *crossing,
                               gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    if (gd_grab_on_hover(s)) {
        gd_grab_keyboard(vc, "grab-on-hover");
    }
    return TRUE;
}

static gboolean gd_leave_event(GtkWidget *widget, GdkEvent *crossing,
                               gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    if (gd_grab_on_hover(s)) {
        gd_ungrab_keyboard(s);
    }
    return TRUE;
}

static gboolean gd_focus_in_event(GtkWidget *widget,
                                  GdkEvent *event, gpointer opaque)
{
    VirtualConsole *vc = opaque;

    win32_kbd_set_window(gd_win32_get_hwnd(vc));
    return TRUE;
}

static gboolean gd_focus_out_event(GtkWidget *widget,
                                   GdkEvent *event, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    win32_kbd_set_window(NULL);
    gtk_release_modifiers(s);
    return TRUE;
}

static gboolean gd_configure(GtkWidget *widget,
                             GdkEvent *cfg, gpointer opaque)
{
    // TODO How about configure events? Window property notification?
    //VirtualConsole *vc = opaque;
    //gd_set_ui_info(vc, cfg->width, cfg->height);
    return FALSE;
}

/** Virtual Console Callbacks **/

static GList *gd_vc_menu_init(GtkDisplayState *s, VirtualConsole *vc,
                               int idx, GList *group, GtkWidget *view_menu)
{
    vc->menu_item = gtk_check_button_new_with_mnemonic(vc->label);
    
    gtk_check_button_set_group((GtkCheckButton*)vc->menu_item, (GtkCheckButton*)group);
    
    GtkShortcutTrigger *trigger = gtk_keyval_trigger_new(GDK_KEY_1 + idx, HOTKEY_MODIFIERS);
    GtkShortcutAction *action = gtk_callback_action_new((GtkShortcutFunc)gd_accel_switch_vc, vc, NULL);
    GtkShortcut *shortcut = gtk_shortcut_new(trigger, action);
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(s->shortcut_controller), shortcut);
    
   // gtk_widget_add_controller(gtk_check_button_get_child(GTK_BUTTON(vc->menu_item)),
    //    (GtkEventController*)s->shortcut_controller
    //);

    //gtk_accel_label_set_accel(
    //        GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BUTTON(vc->menu_item))),
    //        GDK_KEY_1 + idx, HOTKEY_MODIFIERS);

    //g_signal_connect(vc->menu_item, "activate",
    //                 G_CALLBACK(gd_menu_switch_vc), s);
    //gtk_popover_menu_add_child(GTK_POPOVER_MENU(view_menu), vc->menu_item, NULL);

    return NULL;
}

#if defined(CONFIG_VTE)
static void gd_menu_copy(/*GtkMenuItem*/void *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

#if VTE_CHECK_VERSION(0, 50, 0)
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(vc->vte.terminal),
                                       VTE_FORMAT_TEXT);
#else
    vte_terminal_copy_clipboard(VTE_TERMINAL(vc->vte.terminal));
#endif
}

static void gd_vc_adjustment_changed(GtkAdjustment *adjustment, void *opaque)
{
    VirtualConsole *vc = opaque;

    if (gtk_adjustment_get_upper(adjustment) >
        gtk_adjustment_get_page_size(adjustment)) {
        gtk_widget_show(vc->vte.scrollbar);
    } else {
        gtk_widget_hide(vc->vte.scrollbar);
    }
}

static int gd_vc_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    vte_terminal_feed(VTE_TERMINAL(vc->vte.terminal), (const char *)buf, len);
    return len;
}

static void gd_vc_chr_set_echo(Chardev *chr, bool echo)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    if (vc) {
        vc->vte.echo = echo;
    } else {
        vcd->echo = echo;
    }
}

static int nb_vcs;
static Chardev *vcs[MAX_VCS];
static void gd_vc_open(Chardev *chr,
                       ChardevBackend *backend,
                       bool *be_opened,
                       Error **errp)
{
    if (nb_vcs == MAX_VCS) {
        error_setg(errp, "Maximum number of consoles reached");
        return;
    }

    vcs[nb_vcs++] = chr;

    /* console/chardev init sometimes completes elsewhere in a 2nd
     * stage, so defer OPENED events until they are fully initialized
     */
    *be_opened = false;
}

static void char_gd_vc_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->parse = qemu_chr_parse_vc;
    cc->open = gd_vc_open;
    cc->chr_write = gd_vc_chr_write;
    cc->chr_set_echo = gd_vc_chr_set_echo;
}

static const TypeInfo char_gd_vc_type_info = {
    .name = TYPE_CHARDEV_VC,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(VCChardev),
    .class_init = char_gd_vc_class_init,
};

static gboolean gd_vc_in(VteTerminal *terminal, gchar *text, guint size,
                         gpointer user_data)
{
    VirtualConsole *vc = user_data;

    if (vc->vte.echo) {
        VteTerminal *term = VTE_TERMINAL(vc->vte.terminal);
        int i;
        for (i = 0; i < size; i++) {
            uint8_t c = text[i];
            if (c >= 128 || isprint(c)) {
                /* 8-bit characters are considered printable.  */
                vte_terminal_feed(term, &text[i], 1);
            } else if (c == '\r' || c == '\n') {
                vte_terminal_feed(term, "\r\n", 2);
            } else {
                char ctrl[2] = { '^', 0};
                ctrl[1] = text[i] ^ 64;
                vte_terminal_feed(term, ctrl, 2);
            }
        }
    }

    int remaining = size;
    uint8_t* p = (uint8_t *)text;
    while (remaining > 0) {
        int can_write = qemu_chr_be_can_write(vc->vte.chr);
        int written = MIN(remaining, can_write);
        qemu_chr_be_write(vc->vte.chr, p, written);

        remaining -= written;
        p += written;
    }
    return TRUE;
}

static GSList *gd_vc_vte_init(GtkDisplayState *s, VirtualConsole *vc,
                              Chardev *chr, int idx,
                              GSList *group, GtkWidget *view_menu)
{
    char buffer[32];
    GtkWidget *box;
    GtkWidget *scrollbar;
    GtkAdjustment *vadjustment;
    VCChardev *vcd = VC_CHARDEV(chr);

    vc->s = s;
    vc->vte.echo = vcd->echo;
    vc->vte.chr = chr;
    vcd->console = vc;

    snprintf(buffer, sizeof(buffer), "vc%d", idx);
    vc->label = g_strdup_printf("%s", vc->vte.chr->label
                                ? vc->vte.chr->label : buffer);
    group = gd_vc_menu_init(s, vc, idx, group, view_menu);

    vc->vte.terminal = vte_terminal_new();
    g_signal_connect(vc->vte.terminal, "commit", G_CALLBACK(gd_vc_in), vc);

    /* The documentation says that the default is UTF-8, but actually it is
     * 7-bit ASCII at least in VTE 0.38. The function is deprecated since
     * VTE 0.54 (only UTF-8 is supported now). */
#if !VTE_CHECK_VERSION(0, 54, 0)
#if VTE_CHECK_VERSION(0, 38, 0)
    vte_terminal_set_encoding(VTE_TERMINAL(vc->vte.terminal), "UTF-8", NULL);
#else
    vte_terminal_set_encoding(VTE_TERMINAL(vc->vte.terminal), "UTF-8");
#endif
#endif

    vte_terminal_set_scrollback_lines(VTE_TERMINAL(vc->vte.terminal), -1);
    vte_terminal_set_size(VTE_TERMINAL(vc->vte.terminal),
                          VC_TERM_X_MIN, VC_TERM_Y_MIN);

#if VTE_CHECK_VERSION(0, 28, 0)
    vadjustment = gtk_scrollable_get_vadjustment
        (GTK_SCROLLABLE(vc->vte.terminal));
#else
    vadjustment = vte_terminal_get_adjustment(VTE_TERMINAL(vc->vte.terminal));
#endif

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, vadjustment);

    gtk_box_pack_end(GTK_BOX(box), scrollbar, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), vc->vte.terminal, TRUE, TRUE, 0);

    vc->vte.box = box;
    vc->vte.scrollbar = scrollbar;

    g_signal_connect(vadjustment, "changed",
                     G_CALLBACK(gd_vc_adjustment_changed), vc);

    vc->type = GD_VC_VTE;
    vc->tab_item = box;
    vc->focus = vc->vte.terminal;
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), vc->tab_item,
                             gtk_label_new(vc->label));

    qemu_chr_be_event(vc->vte.chr, CHR_EVENT_OPENED);

    return group;
}

static void gd_vcs_init(GtkDisplayState *s, GSList *group,
                        GtkWidget *view_menu)
{
    int i;

    for (i = 0; i < nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[s->nb_vcs];
        group = gd_vc_vte_init(s, vc, vcs[i], s->nb_vcs, group, view_menu);
        s->nb_vcs++;
    }
}
#endif /* CONFIG_VTE */

/** Window Creation **/

static void gd_connect_vc_gfx_signals(VirtualConsole *vc)
{
    // TODO invalid signals
    // draw, event, button-press-event, button-release-event, scroll-event, key-press-event, key-release-event, enter-notify-event, leave-notify-event, focus-in-event, configure-event, grab-broken-event
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(vc->gfx.drawing_area), gd_draw_event, vc, NULL);
#if defined(CONFIG_OPENGL)
    if (gtk_use_gl_area) {
        /* wire up GtkGlArea events */
        g_signal_connect(vc->gfx.drawing_area, "render",
                         G_CALLBACK(gd_render_event), vc);

        g_signal_connect(vc->gfx.drawing_area, "resize", // TODO or connect_after?
                         G_CALLBACK(gd_resize_event), vc);
    }
#endif
   // if (qemu_console_is_graphic(vc->gfx.dcl.con)) {
   //     g_signal_connect(vc->gfx.drawing_area, "event",
   //                      G_CALLBACK(gd_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "button-press-event",
   //                      G_CALLBACK(gd_button_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "button-release-event",
   //                      G_CALLBACK(gd_button_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "scroll-event",
   //                      G_CALLBACK(gd_scroll_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "key-press-event",
   //                      G_CALLBACK(gd_key_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "key-release-event",
   //                      G_CALLBACK(gd_key_event), vc);
//
   //     g_signal_connect(vc->gfx.drawing_area, "enter-notify-event",
   //                      G_CALLBACK(gd_enter_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "leave-notify-event",
   //                      G_CALLBACK(gd_leave_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "focus-in-event",
   //                      G_CALLBACK(gd_focus_in_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "focus-out-event",
   //                      G_CALLBACK(gd_focus_out_event), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "configure-event",
   //                      G_CALLBACK(gd_configure), vc);
   //     g_signal_connect(vc->gfx.drawing_area, "grab-broken-event",
   //                      G_CALLBACK(gd_grab_broken_event), vc);
   // } else {
   //     g_signal_connect(vc->gfx.drawing_area, "key-press-event",
   //                      G_CALLBACK(gd_text_key_down), vc);
   // }
}

static void gd_connect_signals(GtkDisplayState *s)
{
    s->machine_actions = g_simple_action_group_new();
    s->view_actions = g_simple_action_group_new();

   // g_signal_connect(s->show_tabs_item, "activate",
   //                  G_CALLBACK(gd_menu_show_tabs), s);
   // g_signal_connect(s->untabify_item, "activate",
   //                  G_CALLBACK(gd_menu_untabify), s);
   // g_signal_connect(s->show_menubar_item, "activate",
   //                  G_CALLBACK(gd_menu_show_menubar), s);

    g_signal_connect(s->window, "close-request",
                     G_CALLBACK(gd_window_close), s);

    s->pause_state = g_variant_new_boolean(FALSE);
    GSimpleAction *act_pause = g_simple_action_new_stateful("pause", NULL, s->pause_state);
    g_signal_connect(G_OBJECT(act_pause), "activate", G_CALLBACK(gd_menu_pause), s);
    g_action_map_add_action(G_ACTION_MAP(s->machine_actions), G_ACTION(act_pause));

    GSimpleAction *act_reset = g_simple_action_new("reset", NULL);
    g_signal_connect(act_reset, "activate", G_CALLBACK(gd_menu_reset), s);
    g_action_map_add_action(G_ACTION_MAP(s->machine_actions), G_ACTION(act_reset));

    GSimpleAction *act_powerdown = g_simple_action_new("powerdown", NULL);
    g_signal_connect(act_powerdown, "activate", G_CALLBACK(gd_menu_powerdown), s);
    g_action_map_add_action(G_ACTION_MAP(s->machine_actions), G_ACTION(act_powerdown));

    GSimpleAction *act_quit = g_simple_action_new("quit", NULL);
    g_signal_connect(act_quit, "activate", G_CALLBACK(gd_menu_quit), s);
    g_action_map_add_action(G_ACTION_MAP(s->machine_actions), G_ACTION(act_quit));

                     /*
#if defined(CONFIG_VTE)
    g_signal_connect(s->copy_item, "activate",
                     G_CALLBACK(gd_menu_copy), s);
#endif
*/

    GSimpleAction *act_fullscreen = g_simple_action_new("fullscreen", NULL);
    g_signal_connect(act_fullscreen, "activate", G_CALLBACK(gd_menu_full_screen), s);
    g_action_map_add_action(G_ACTION_MAP(s->view_actions), G_ACTION(act_fullscreen));

/*
    g_signal_connect(s->zoom_in_item, "activate",
                     G_CALLBACK(gd_menu_zoom_in), s);
    g_signal_connect(s->zoom_out_item, "activate",
                     G_CALLBACK(gd_menu_zoom_out), s);
    g_signal_connect(s->zoom_fixed_item, "activate",
                     G_CALLBACK(gd_menu_zoom_fixed), s);
    g_signal_connect(s->zoom_fit_item, "activate",
                     G_CALLBACK(gd_menu_zoom_fit), s);
    g_signal_connect(s->grab_item, "activate",
                     G_CALLBACK(gd_menu_grab_input), s);
    g_signal_connect(s->notebook, "switch-page",
                     G_CALLBACK(gd_change_page), s);
    */
}

static GtkWidget *gd_create_menu_machine(GtkDisplayState *s)
{
    GtkWidget *machine_menu;
    //GtkWidget *separator;
    GMenu *model = g_menu_new();
    //gtk_widget_add_controller(machine_menu, (GtkEventController*)s->shortcut_controller);

    GMenuItem* pause_item = g_menu_item_new(_("_Pause"), NULL);
   // s->pause_item = gtk_menu_button_new(/*_("_Pause")*/);
    g_menu_append_item(model, G_MENU_ITEM(pause_item));

    //separator = gtk_separator_menu_item_new();
    //gtk_menu_shell_append(GTK_POPOVER_MENU(machine_menu), separator);

    //s->reset_item = gtk_button_new_with_mnemonic(_("_Reset"));
    //gtk_menu_shell_append(GTK_POPOVER_MENU(machine_menu), s->reset_item);

    //s->powerdown_item = gtk_button_new_with_mnemonic(_("Power _Down"));
    //gtk_menu_shell_append(GTK_POPOVER_MENU(machine_menu), s->powerdown_item);

    //separator = gtk_separator_menu_item_new();
    //gtk_menu_shell_append(GTK_POPOVER_MENU(machine_menu), separator);

    //s->quit_item = gtk_button_new_with_mnemonic(_("_Quit"));
    //gtk_button_set_accel_path(GTK_BUTTON(s->quit_item),
    //                             "<QEMU>/Machine/Quit");
    //gtk_accel_map_add_entry("<QEMU>/Machine/Quit",
    //                        GDK_KEY_q, HOTKEY_MODIFIERS);
    //gtk_menu_shell_append(GTK_POPOVER_MENU(machine_menu), s->quit_item);

    machine_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
    return machine_menu;
}

#if defined(CONFIG_OPENGL)
static void gl_area_realize(GtkGLArea *area, VirtualConsole *vc)
{
    gtk_gl_area_make_current(area);
    qemu_egl_display = eglGetCurrentDisplay();
    vc->gfx.has_dmabuf = qemu_egl_has_dmabuf();
    if (!vc->gfx.has_dmabuf) {
        error_report("GtkGLArea console lacks DMABUF support.");
    }
}
#endif

static GList *gd_vc_gfx_init(GtkDisplayState *s, VirtualConsole *vc,
                              QemuConsole *con, int idx,
                              GList *group, GtkWidget *view_menu)
{
    bool zoom_to_fit = false;

    vc->label = qemu_console_get_label(con);
    vc->s = s;
    vc->gfx.scale_x = 1.0;
    vc->gfx.scale_y = 1.0;

#if defined(CONFIG_OPENGL)
    if (display_opengl) {
        if (gtk_use_gl_area) {
            vc->gfx.drawing_area = gtk_gl_area_new();
            g_signal_connect(vc->gfx.drawing_area, "realize",
                             G_CALLBACK(gl_area_realize), vc);
            vc->gfx.dcl.ops = &dcl_gl_area_ops;
        } else {
#ifdef CONFIG_X11
            vc->gfx.drawing_area = gtk_drawing_area_new();
            /*
             * gtk_widget_set_double_buffered() was deprecated in 3.14.
             * It is required for opengl rendering on X11 though.  A
             * proper replacement (native opengl support) is only
             * available in 3.16+.  Silence the warning if possible.
             */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            //gtk_widget_set_double_buffered(vc->gfx.drawing_area, FALSE);
#pragma GCC diagnostic pop
            vc->gfx.dcl.ops = &dcl_egl_ops;
            vc->gfx.has_dmabuf = qemu_egl_has_dmabuf();
#else
            abort();
#endif
        }
    } else
#endif
    {
        vc->gfx.drawing_area = gtk_drawing_area_new();
        vc->gfx.dcl.ops = &dcl_ops;
    }


    // TODO figure out controllers for the various mask below
    GtkEventController* motion = gtk_event_controller_motion_new();
    gtk_widget_add_controller(
        vc->gfx.drawing_area,
        motion
    );
    //gtk_widget_add_events(vc->gfx.drawing_area,
    //                      GDK_POINTER_MOTION_MASK |
    //                      GDK_BUTTON_PRESS_MASK |
    //                      GDK_BUTTON_RELEASE_MASK |
    //                      GDK_BUTTON_MOTION_MASK |
    //                      GDK_ENTER_NOTIFY_MASK |
    //                      GDK_LEAVE_NOTIFY_MASK |
    //                      GDK_SCROLL_MASK |
    //                      GDK_SMOOTH_SCROLL_MASK |
    //                      GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus(vc->gfx.drawing_area, TRUE);

    vc->type = GD_VC_GFX;
    vc->tab_item = vc->gfx.drawing_area;
    vc->focus = vc->gfx.drawing_area;
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook),
                             vc->tab_item, gtk_label_new(vc->label));

    vc->gfx.kbd = qkbd_state_init(con);
    vc->gfx.dcl.con = con;

    register_displaychangelistener(&vc->gfx.dcl);

    gd_connect_vc_gfx_signals(vc);
    group = gd_vc_menu_init(s, vc, idx, group, view_menu);

    if (dpy_ui_info_supported(vc->gfx.dcl.con)) {
        zoom_to_fit = true;
    }
    if (s->opts->u.gtk.has_zoom_to_fit) {
        zoom_to_fit = s->opts->u.gtk.zoom_to_fit;
    }
    if (zoom_to_fit) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(s->zoom_fit_item), true);
        s->free_scale = true;
    }

    return group;
}

static GtkWidget *gd_create_menu_view(GtkDisplayState *s)
{
    GList *group = NULL;
    GtkWidget *view_menu;
    GtkWidget *separator;
    QemuConsole *con;
    int vc;

    GMenu* menu_model = g_menu_new();
    GMenuItem* fullscreen_item = g_menu_item_new("Fullscreen", NULL);
    g_menu_append_item(menu_model, fullscreen_item);

    view_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_model));
    gtk_widget_add_controller(view_menu, s->shortcut_controller);

    s->full_screen_item = gtk_check_button_new_with_mnemonic(_("_Fullscreen"));

#if defined(CONFIG_VTE)
    s->copy_item = gtk_button_new_with_mnemonic(_("_Copy"));
    gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->copy_item);
#endif


    GtkShortcutTrigger* trigger = gtk_keyval_trigger_new(GDK_KEY_f, HOTKEY_MODIFIERS);
    GtkShortcutAction* action = gtk_callback_action_new((GtkShortcutFunc)gd_accel_full_screen, s, NULL);
    GtkShortcut* shortcut = gtk_shortcut_new(trigger, action);
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(s->shortcut_controller), shortcut);
    
    GtkShortcutLabel *shortcut_label = gtk_shortcut_label_new("f");
    gtk_button_set_child(GTK_BUTTON(s->full_screen_item), shortcut_label);
    gtk_popover_menu_add_child(GTK_POPOVER_MENU(view_menu), s->full_screen_item, "Fullscreen");

    separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    //gtk_popover_menu_add_child(GTK_POPOVER_MENU(view_menu), separator, NULL);

    s->zoom_in_item = gtk_check_button_new_with_mnemonic(_("Zoom _In"));
    //gtk_button_set_accel_path(GTK_BUTTON(s->zoom_in_item),
    //                             "<QEMU>/View/Zoom In");
    //gtk_accel_map_add_entry("<QEMU>/View/Zoom In", GDK_KEY_plus,
    //                        HOTKEY_MODIFIERS);
    //gtk_shortcut_controller_connect(s->shortcut_controller, GDK_KEY_equal, HOTKEY_MODIFIERS, 0,
    //        g_cclosure_new_swap(G_CALLBACK(gd_accel_zoom_in), s, NULL));
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->zoom_in_item);

    //s->zoom_out_item = gtk_button_new_with_mnemonic(_("Zoom _Out"));
    //gtk_button_set_accel_path(GTK_BUTTON(s->zoom_out_item),
    //                             "<QEMU>/View/Zoom Out");
    //gtk_accel_map_add_entry("<QEMU>/View/Zoom Out", GDK_KEY_minus,
    //                        HOTKEY_MODIFIERS);
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->zoom_out_item);

    //s->zoom_fixed_item = gtk_button_new_with_mnemonic(_("Best _Fit"));
    //gtk_button_set_accel_path(GTK_BUTTON(s->zoom_fixed_item),
    //                             "<QEMU>/View/Zoom Fixed");
    //gtk_accel_map_add_entry("<QEMU>/View/Zoom Fixed", GDK_KEY_0,
    //                        HOTKEY_MODIFIERS);
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->zoom_fixed_item);

    s->zoom_fit_item = gtk_check_button_new_with_mnemonic(_("Zoom To _Fit"));
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->zoom_fit_item);

    separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    //gtk_popover_menu_add_child(GTK_POPOVER_MENU(view_menu), separator, NULL);

    //s->grab_on_hover_item = gtk_check_button_new_with_mnemonic(_("Grab On _Hover"));
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->grab_on_hover_item);
//
    //s->grab_item = gtk_check_button_new_with_mnemonic(_("_Grab Input"));
    //gtk_button_set_accel_path(GTK_BUTTON(s->grab_item),
    //                             "<QEMU>/View/Grab Input");
    //gtk_accel_map_add_entry("<QEMU>/View/Grab Input", GDK_KEY_g,
    //                        HOTKEY_MODIFIERS);
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->grab_item);

    separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    //gtk_popover_menu_add_child(GTK_POPOVER_MENU(view_menu), separator, NULL);

    /* gfx */
    for (vc = 0;; vc++) {
        con = qemu_console_lookup_by_index(vc);
        if (!con) {
            break;
        }
        group = gd_vc_gfx_init(s, &s->vc[vc], con,
                               vc, group, view_menu);
        s->nb_vcs++;
    }

#if defined(CONFIG_VTE)
    /* vte */
    gd_vcs_init(s, group, view_menu);
#endif

    separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    //gtk_popover_menu_add_child(GTK_POPOVER_MENU(view_menu), separator, NULL);

    //s->show_tabs_item = gtk_check_button_new_with_mnemonic(_("Show _Tabs"));
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->show_tabs_item);
//
    //s->untabify_item = gtk_button_new_with_mnemonic(_("Detach Tab"));
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->untabify_item);
//
    //s->show_menubar_item = gtk_check_button_new_with_mnemonic(
    //        _("Show Menubar"));
    //gtk_check_button_set_active(GTK_CHECK_BUTTON(s->show_menubar_item),
    //                               TRUE);
    //gtk_shortcut_controller_connect(s->shortcut_controller, GDK_KEY_m, HOTKEY_MODIFIERS, 0,
    //        g_cclosure_new_swap(G_CALLBACK(gd_accel_show_menubar), s, NULL));
    //gtk_accel_label_set_accel(
    //        GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BUTTON(s->show_menubar_item))),
    //        GDK_KEY_m, HOTKEY_MODIFIERS);
    //gtk_menu_shell_append(GTK_POPOVER_MENU(view_menu), s->show_menubar_item);

    return view_menu;
}

static void gd_create_menus(GtkDisplayState *s)
{
    GtkSettings *settings;

    s->shortcut_controller = gtk_shortcut_controller_new(); // <- s->shortcut_controller = gtk_shortcut_controller_new();
    //s->machine_menu = gd_create_menu_machine(s);
    s->view_menu = gd_create_menu_view(s);
//
    //s->machine_menu_item = gtk_button_new_with_mnemonic(_("_Machine"));
    //gtk_button_set_child(GTK_BUTTON(s->machine_menu_item),
    //                          s->machine_menu);
    ////gtk_popover_menu_bar_add_child(GTK_POPOVER_MENU_BAR(s->menu_bar), s->machine_menu_item, "machine");
//
    //s->view_menu_item = gtk_button_new_with_mnemonic(_("_View"));
    //gtk_button_set_child(GTK_BUTTON(s->view_menu_item), s->view_menu);
    //gtk_popover_menu_bar_add_child(GTK_POPOVER_MENU_BAR(s->menu_bar), s->view_menu_item, "view");

    g_object_set_data(G_OBJECT(s->surface), "shortcut_controller", s->shortcut_controller);
    gtk_widget_add_controller(s->window, s->shortcut_controller); // <- gtk_window_add_shortcut_controller(GTK_WINDOW(s->window), s->shortcut_controller);

    /* Disable the default "F10" menu shortcut. */
    // TODO how do we disable this shortcut, do we want to? Deprecated?
    //settings = gtk_widget_get_settings(s->window);
    //g_object_set(G_OBJECT(settings), "gtk-menu-bar-accel", "", NULL);
}


static GMenu *gd_create_machine_menu_model(GtkDisplayState *s) {
    GMenu *machine_menu = g_menu_new();

    GMenu* first = g_menu_new();
    GMenuItem* pause_item = g_menu_item_new(_("_Pause"), "machine.pause");
    g_menu_append_item(first, pause_item);
    g_object_unref(pause_item);

    GMenu* second = g_menu_new();

    GMenuItem *reset_item = g_menu_item_new(_("_Reset"), "machine.reset");
    g_menu_append_item(second, reset_item);
    g_object_unref(reset_item);

    GMenuItem *powerdown_item = g_menu_item_new(_("Power _Down"), "machine.powerdown");
    g_menu_append_item(second, powerdown_item);
    g_object_unref(powerdown_item);

    GMenu* third = g_menu_new();

    GMenuItem *quit_item = g_menu_item_new(_("_Quit"), "machine.quit");
    g_menu_item_set_attribute_value(quit_item, "accel", g_variant_new_string("<Ctrl><Alt>Q"));
    g_menu_append_item(third, quit_item);
    g_object_unref(quit_item);

    g_menu_append_section(machine_menu, NULL, first);
    g_menu_append_section(machine_menu, NULL, second);
    g_menu_append_section(machine_menu, NULL, third);

    g_object_unref(first);
    g_object_unref(second);
    g_object_unref(third);

    return machine_menu;
}

static GMenu *gd_create_view_menu_model(GtkDisplayState *s) {
    GMenu *view_menu = g_menu_new();

    GMenuItem* fullscreen_item = g_menu_item_new(_("_Fullscreen"), "view.fullscreen");
    g_menu_append_item(view_menu, fullscreen_item);
    g_object_unref(fullscreen_item);

    return view_menu;
}

static gboolean gtkinit;

static void gtk_display_init(DisplayState *ds, DisplayOptions *opts)
{
    VirtualConsole *vc;

    GtkDisplayState *s = g_malloc0(sizeof(*s));
    GdkDisplay *window_display;
    GtkIconTheme *theme;
    char *dir[2] = {NULL, NULL};

    if (!gtkinit) {
        fprintf(stderr, "gtk initialization failed\n");
        exit(1);
    }
    assert(opts->type == DISPLAY_TYPE_GTK);
    s->opts = opts;

    theme = gtk_icon_theme_new();
    dir[0] = get_relocated_path(CONFIG_QEMU_ICONDIR);
    gtk_icon_theme_set_search_path(theme, (const char* const*)dir);
    g_free(dir[0]);
    g_set_prgname("qemu");

    s->window = gtk_window_new();
    window_display = gtk_widget_get_display(s->window);
    s->surface = gdk_surface_new_toplevel(window_display);
    s->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    s->notebook = gtk_notebook_new();

    // Describe actions first
    gd_connect_signals(s);

    // TODO: menu bar not needed if we use the headerbar
    //s->menu_bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menu_model));
    //gtk_widget_insert_action_group(s->menu_bar, "view", s->view_actions);
    
    GtkWidget *titlebar = gtk_header_bar_new();

    GtkButton *machine_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(machine_button, "applications-system");
    GMenu* machine_menu_model = gd_create_machine_menu_model(s);
    gtk_menu_button_set_menu_model(GTK_BUTTON(machine_button), machine_menu_model);
    g_object_unref(machine_menu_model);
    gtk_widget_insert_action_group(machine_button, "machine", s->machine_actions);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), machine_button);

    GtkButton *view_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(view_button, "applications-graphics");
    GMenu* view_menu_model = gd_create_view_menu_model(s);
    gtk_menu_button_set_menu_model(GTK_BUTTON(view_button), view_menu_model);
    g_object_unref(view_menu_model);
    gtk_widget_insert_action_group(view_button, "view", s->view_actions);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), view_button);

    gtk_window_set_titlebar(GTK_WINDOW(s->window), titlebar);

    s->free_scale = FALSE;

    /* Mostly LC_MESSAGES only. See early_gtk_display_init() for details. For
     * LC_CTYPE, we need to make sure that non-ASCII characters are considered
     * printable, but without changing any of the character classes to make
     * sure that we don't accidentally break implicit assumptions.  */
    setlocale(LC_MESSAGES, "");
    setlocale(LC_CTYPE, "C.UTF-8");
    dir[0] = get_relocated_path(CONFIG_QEMU_LOCALEDIR);
    bindtextdomain("qemu", (const char* ) dir[0]);
    g_free(dir[0]);
    bind_textdomain_codeset("qemu", "UTF-8");
    textdomain("qemu");

    if (s->opts->has_show_cursor && s->opts->show_cursor) {
        s->null_cursor = NULL; /* default pointer */
    } else {
        s->null_cursor = NULL;//gdk_cursor_new_from_texture(NULL, 0, 0, NULL);
    }

    s->mouse_mode_notifier.notify = gd_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&s->mouse_mode_notifier);
    qemu_add_vm_change_state_handler(gd_change_runstate, s);

    gtk_window_set_icon_name(GTK_WINDOW(s->window), "qemu");

    gd_create_menus(s);

    //gd_connect_signals(s); // moved up

    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s->notebook), FALSE);

    gd_update_caption(s);

    // TODO what do they do?
    //gtk_box_append(GTK_BOX(s->vbox), s->menu_bar);
    gtk_box_append(GTK_BOX(s->vbox), s->notebook);
    //gtk_box_pack_start(GTK_BOX(s->vbox), s->menu_bar, FALSE, TRUE, 0);
    //gtk_box_pack_start(GTK_BOX(s->vbox), s->notebook, TRUE, TRUE, 0);

    gtk_window_set_child(GTK_WINDOW(s->window), s->vbox); //<- gtk_container_add(GTK_CONTAINER(s->window), s->vbox);

    gtk_widget_show(s->window);

    vc = gd_vc_find_current(s);
    gtk_widget_set_sensitive(s->view_menu, vc != NULL);
#ifdef CONFIG_VTE
    gtk_widget_set_sensitive(s->copy_item,
                             vc && vc->type == GD_VC_VTE);
#endif

    if (opts->has_full_screen &&
        opts->full_screen) {
        //gtk_button_activate(GTK_BUTTON(s->full_screen_item));
    }
    if (opts->u.gtk.has_grab_on_hover &&
        opts->u.gtk.grab_on_hover) {
        //gtk_button_activate(GTK_BUTTON(s->grab_on_hover_item));
    }
    gd_clipboard_init(s);
}

static void early_gtk_display_init(DisplayOptions *opts)
{
    /* The QEMU code relies on the assumption that it's always run in
     * the C locale. Therefore it is not prepared to deal with
     * operations that produce different results depending on the
     * locale, such as printf's formatting of decimal numbers, and
     * possibly others.
     *
     * Since GTK+ calls setlocale() by default -importing the locale
     * settings from the environment- we must prevent it from doing so
     * using gtk_disable_setlocale().
     *
     * QEMU's GTK+ UI, however, _does_ have translations for some of
     * the menu items. As a trade-off between a functionally correct
     * QEMU and a fully internationalized UI we support importing
     * LC_MESSAGES from the environment (see the setlocale() call
     * earlier in this file). This allows us to display translated
     * messages leaving everything else untouched.
     */
    gtk_disable_setlocale();
    gtkinit = gtk_init_check();
    if (!gtkinit) {
        /* don't exit yet, that'll break -help */
        return;
    }

    assert(opts->type == DISPLAY_TYPE_GTK);
    if (opts->has_gl && opts->gl != DISPLAYGL_MODE_OFF) {
#if defined(CONFIG_OPENGL)
#if defined(GDK_WINDOWING_WAYLAND)
        if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
            gtk_use_gl_area = true;
            gtk_gl_area_init();
        } else
#endif
        {
#ifdef CONFIG_X11
            DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAYGL_MODE_ON;
            gtk_egl_init(mode);
#endif
        }
#endif
    }

    keycode_map = gd_get_keymap(&keycode_maplen);

#if defined(CONFIG_VTE)
    type_register(&char_gd_vc_type_info);
#endif
}

static QemuDisplay qemu_display_gtk = {
    .type       = DISPLAY_TYPE_GTK,
    .early_init = early_gtk_display_init,
    .init       = gtk_display_init,
};

static void register_gtk(void)
{
    qemu_display_register(&qemu_display_gtk);
}

type_init(register_gtk);
