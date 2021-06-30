/*
 * GTK UI -- egl opengl code.
 *
 * Note that gtk 3.16+ (released 2015-03-23) has a GtkGLArea widget,
 * which is GtkDrawingArea like widget with opengl rendering support.
 *
 * This code handles opengl support on older gtk versions, using egl
 * to get a opengl context for the X11 window.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "trace.h"

#include "ui/console.h"
#include "ui/gtk.h"
#include "ui/egl-helpers.h"
#include "ui/vulkan-helpers.h"
#include "ui/shader.h"

#include "sysemu/sysemu.h"

static void gtk_vk_set_scanout_mode(VirtualConsole *vc, bool scanout)
{
    if (vc->gfx.scanout_mode == scanout) {
        return;
    }

    vc->gfx.scanout_mode = scanout;
    if (!vc->gfx.scanout_mode) {
        vk_fb_destroy(vc->gfx.vk_device, &vc->gfx.guest_vk_fb);
        if (vc->gfx.surface) {
            // TODO destroy create vk texture (or framebuffer?)
            surface_vk_destroy_texture(vc->gfx.vk_device, vc->gfx.ds);
            surface_vk_create_texture(vc->gfx.vk_device, vc->gfx.vk_surface, vc->gfx.ds);
        }
    }
}

/** DisplayState Callbacks (opengl version) **/

void gd_vk_init(VirtualConsole *vc)
{
    GdkWindow *gdk_window = gtk_widget_get_window(vc->gfx.drawing_area);
    if (!gdk_window) {
        return;
    }

    Window x11_window = gdk_x11_window_get_xid(gdk_window);
    if (!x11_window) {
        return;
    }

    GdkDisplay *gdk_display = gdk_window_get_display(gdk_window);
    Display *dpy = gdk_x11_display_get_xdisplay(gdk_display);

    vc->gfx.vk_instance = vk_create_instance();
    vc->gfx.vk_physical_device = vk_create_physical_device(vc->gfx.vk_instance);
    vc->gfx.vk_device = vk_create_device(vc->gfx.vk_instance, vc->gfx.vk_physical_device);
    vc->gfx.vk_surface = qemu_vk_init_surface_x11(vc->gfx.vk_instance, dpy, x11_window);
    vc->gfx.vk_swapchain = vk_create_swapchain(vc->gfx.vk_physical_device, vc->gfx.vk_device, vc->gfx.vk_surface);

    assert(vc->gfx.vk_surface);
}

void gd_vk_draw(VirtualConsole *vc)
{
    GdkWindow *window;
    int ww, wh;

    if (!vc->gfx.vks) {
        return;
    }

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    ww = gdk_window_get_width(window);
    wh = gdk_window_get_height(window);

    if (vc->gfx.scanout_mode) {
        gd_vk_scanout_flush(&vc->gfx.dcl, 0, 0, vc->gfx.w, vc->gfx.h);

        vc->gfx.scale_x = (double)ww / vc->gfx.w;
        vc->gfx.scale_y = (double)wh / vc->gfx.h;
    } else {
        if (!vc->gfx.ds) {
            return;
        }
        /*
        eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                       vc->gfx.esurface, vc->gfx.ectx);

        surface_gl_setup_viewport(vc->gfx.gls, vc->gfx.ds, ww, wh);
        surface_gl_render_texture(vc->gfx.gls, vc->gfx.ds);

        eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
        */

        vc->gfx.scale_x = (double)ww / surface_width(vc->gfx.ds);
        vc->gfx.scale_y = (double)wh / surface_height(vc->gfx.ds);
    }

    //glFlush();
    //graphic_hw_gl_flushed(vc->gfx.dcl.con);
}

void gd_vk_update(DisplayChangeListener *dcl,
                   int x, int y, int w, int h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (!vc->gfx.vks || !vc->gfx.ds) {
        return;
    }

    // TODO recreate swapchain?
/*
    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);
    surface_gl_update_texture(vc->gfx.gls, vc->gfx.ds, x, y, w, h);
    vc->gfx.glupdates++;
    */
}

void gd_vk_refresh(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.dcl.update_interval = gd_monitor_update_interval(
            vc->window ? vc->window : vc->gfx.drawing_area);

// TODO: vulkan surface?

    if (!vc->gfx.vk_surface) {
        gd_vk_init(vc);
        if (!vc->gfx.vk_surface) {
            return;
        }
        vc->gfx.vks = qemu_vk_init_shader(vc->gfx.vk_device);
        if (vc->gfx.ds) {
           surface_vk_create_texture(vc->gfx.vk_device, vc->gfx.vk_surface, vc->gfx.ds);
        }
    }

    graphic_hw_update(dcl->con);

    if (vc->gfx.glupdates) {
        vc->gfx.glupdates = 0;
        gtk_vk_set_scanout_mode(vc, false);
        gd_vk_draw(vc);
    }
    
}

void gd_vk_switch(DisplayChangeListener *dcl,
                  DisplaySurface *surface)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }

    surface_vk_destroy_texture(vc->gfx.vk_device, vc->gfx.ds);
    vc->gfx.ds = surface;
    if (vc->gfx.vks) {
        surface_vk_create_texture(vc->gfx.vk_device, vc->gfx.vk_surface, vc->gfx.ds);
    }

    if (resized) {
        gd_update_windowsize(vc);
    }
}

QEMUVulkanContext gd_vk_create_context(DisplayChangeListener *dcl)
{
    return vk_create_context();
}

void gd_vk_scanout_disable(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.w = 0;
    vc->gfx.h = 0;
    gtk_vk_set_scanout_mode(vc, false);
}

void gd_vk_scanout_texture(DisplayChangeListener *dcl,
                            vulkan_texture texture, bool backing_y_0_top,
                            uint32_t backing_width, uint32_t backing_height,
                            uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.x = x;
    vc->gfx.y = y;
    vc->gfx.w = w;
    vc->gfx.h = h;
    vc->gfx.y0_top = backing_y_0_top;

    gtk_vk_set_scanout_mode(vc, true);
    vk_fb_setup_for_tex(vc->gfx.vk_device, &vc->gfx.guest_vk_fb, texture);
}

void gd_vk_scanout_dmabuf(DisplayChangeListener *dcl,
                           QemuDmaBuf *dmabuf)
{
#ifdef CONFIG_GBM
/*
    egl_dmabuf_import_texture(dmabuf);
    if (!dmabuf->texture) {
        return;
    }

    gd_egl_scanout_texture(dcl, dmabuf->texture,
                           false, dmabuf->width, dmabuf->height,
                           0, 0, dmabuf->width, dmabuf->height);
                           */
#endif
}

void gd_vk_cursor_dmabuf(DisplayChangeListener *dcl,
                          QemuDmaBuf *dmabuf, bool have_hot,
                          uint32_t hot_x, uint32_t hot_y)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

/*
    if (dmabuf) {
        egl_dmabuf_import_texture(dmabuf);
        if (!dmabuf->texture) {
            return;
        }
        egl_fb_setup_for_tex(&vc->gfx.cursor_fb, dmabuf->width, dmabuf->height,
                             dmabuf->texture, false);
    } else {
        egl_fb_destroy(&vc->gfx.cursor_fb);
    }
    */
#endif
}

void gd_vk_cursor_position(DisplayChangeListener *dcl,
                            uint32_t pos_x, uint32_t pos_y)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.cursor_x = pos_x * vc->gfx.scale_x;
    vc->gfx.cursor_y = pos_y * vc->gfx.scale_y;
}

void gd_vk_release_dmabuf(DisplayChangeListener *dcl,
                           QemuDmaBuf *dmabuf)
{
#ifdef CONFIG_GBM
    //egl_dmabuf_release_texture(dmabuf);
#endif
}

void gd_vk_scanout_flush(DisplayChangeListener *dcl,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkWindow *window;
    int ww, wh;

    if (!vc->gfx.scanout_mode) {
        return;
    }

    if (vc->gfx.guest_vk_fb.framebuffer == VK_NULL_HANDLE) {
        return;
    }

    //eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
    //               vc->gfx.esurface, vc->gfx.ectx);

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    ww = gdk_window_get_width(window);
    wh = gdk_window_get_height(window);
    vk_fb_setup_default(&vc->gfx.win_fb, ww, wh);
    // TODO acquire next image?

    if (vc->gfx.cursor_fb.texture) {
        //vk_texture_blit(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.guest_fb,
        //                 vc->gfx.y0_top);
        //vk_texture_blend(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.cursor_fb,
        //                  vc->gfx.y0_top,
        //                  vc->gfx.cursor_x, vc->gfx.cursor_y,
        //                  vc->gfx.scale_x, vc->gfx.scale_y);
    } else {
        //vk_fb_blit(&vc->gfx.win_fb, &vc->gfx.guest_fb, !vc->gfx.y0_top);
    }

    // TODO: present
    //eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
}

void gtk_vk_init()
{
    GdkDisplay *gdk_display = gdk_display_get_default();
    GdkWindow *gdk_window = gdk_get_default_root_window();

#if defined(GDK_WINDOWING_WAYLAND)
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        struct wl_display *wl_dpy = gdk_wayland_display_get_wl_display(gdk_display);
        struct wl_surface *wl_surface = gdk_wayland_window_get_wl_surface(gdk_window);

        if (qemu_vk_init_dpy_wayland(wl_dpy, wl_surface) < 0) {
            return;
        }
    }

#elif defined(CONFIG_X11)
    if (GDK_IS_X11_DISPLAY(gdk_display)) {
        Display *x_display = gdk_x11_display_get_xdisplay(gdk_display);
        Window x_window = gdk_x11_window_get_xid(gdk_window);

        if (qemu_vk_init_dpy_x11(x_display, x_window) < 0) {
            return;
        }
    }
#endif

    display_vulkan = 1;
}

int gd_vk_make_current(DisplayChangeListener *dcl,
                        QEMUVulkanContext ctx)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    // TODO should we do something in Vulkan? I believe not.
    // return eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
    //                      vc->gfx.esurface, ctx);
    return 0;
}
