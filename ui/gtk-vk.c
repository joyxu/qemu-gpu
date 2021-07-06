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
#include "ui/vulkan-helpers.h"
#include "ui/vulkan-shader.h"

#include "sysemu/sysemu.h"

#define VK_CHECK(res) g_assert(res == VK_SUCCESS)

static void gtk_vk_set_scanout_mode(VirtualConsole *vc, bool scanout)
{
    if (vc->gfx.scanout_mode == scanout) {
        return;
    }

    vc->gfx.scanout_mode = scanout;
    if (!vc->gfx.scanout_mode) {
        vk_fb_destroy(vc->gfx.vk_device.handle, &vc->gfx.guest_vk_fb);
        if (vc->gfx.surface) {
            // TODO destroy create vk texture (or framebuffer?)
            surface_vk_destroy_texture(vc->gfx.vk_device.handle, vc->gfx.ds);
            surface_vk_create_texture(vc->gfx.vk_device, vc->gfx.ds);
        }
    }
}

/** DisplayState Callbacks (opengl version) **/

static VkSurfaceKHR gd_vk_create_surface(VkInstance instance, GdkWindow *gdk_window, GdkDisplay *gdk_display)
{
#if defined(GDK_WINDOWING_WAYLAND)
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        struct wl_display *wl_dpy = gdk_wayland_display_get_wl_display(gdk_display);
        struct wl_surface *wl_surface = gdk_wayland_window_get_wl_surface(gdk_window);

        return vk_create_wayland_surface(instance, wl_dpy, wl_surface);
    }
#endif

#if defined(CONFIG_X11)
    if (GDK_IS_X11_DISPLAY(gdk_display)) {
        Display *x_display = gdk_x11_display_get_xdisplay(gdk_display);
        Window x_window = gdk_x11_window_get_xid(gdk_window);

        return vk_create_x11_surface(instance, x_display, x_window);
    }
#endif

    g_assert(false);
    return VK_NULL_HANDLE;
}

void gd_vk_init(VirtualConsole *vc)
{
    GdkWindow *gdk_window = gtk_widget_get_window(vc->gfx.drawing_area);
    if (!gdk_window) {
        return;
    }

    GdkDisplay *gdk_display = gdk_window_get_display(gdk_window);
    if (!gdk_display) {
        return;
    }

    vc->gfx.vk_instance = vk_create_instance();
    VkSurfaceKHR vk_surface = gd_vk_create_surface(vc->gfx.vk_instance, gdk_window, gdk_display);
    QEMUVkPhysicalDevice physical_device = vk_get_physical_device(vc->gfx.vk_instance, vk_surface);
    vc->gfx.vk_device = vk_create_device(vc->gfx.vk_instance, physical_device);

    uint32_t width = gdk_window_get_width(gdk_window);
    uint32_t height = gdk_window_get_height(gdk_window);
    vc->gfx.vk_swapchain = vk_create_swapchain(vc->gfx.vk_device, VK_NULL_HANDLE, width, height);
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
        
        QEMUVkDevice device = vc->gfx.vk_device;
        QEMUVkSwapchain *swapchain = &vc->gfx.vk_swapchain;
        QEMUVkFrames *frames = &vc->gfx.vk_frames;
        QEMUVulkanShader *shader = vc->gfx.vks;

        VkSemaphore image_available_semaphore = vk_create_semaphore(device.handle);
        VkSemaphore render_finished_semaphore = vk_create_semaphore(device.handle);
        VkFence fence = vk_create_fence(device.handle);

        uint32_t image_index;
        // Image available sempahore will be signaled once the image is correctly acquired
        VkResult acquire_res = vkAcquireNextImageKHR(device.handle, swapchain->handle, UINT64_MAX, image_available_semaphore, VK_NULL_HANDLE, &image_index);
        frames->frame_index = image_index;

        if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR)
        {
            // TODO extract to function
            vk_swapchain_recreate(device, swapchain, 0, 0);

            for (uint32_t i = 0; i < swapchain->image_count; ++i)
            {
                vkDestroyFramebuffer(device.handle, frames->framebuffers[i], NULL);
                frames->framebuffers[i] = vk_create_framebuffer(device.handle, shader->texture_blit_render_pass, swapchain->views[i], swapchain->extent.width, swapchain->extent.height);
            }

            frames->frame_index = 0;

            // TODO Skip this frame? Try acquiring the swapchain again?
            return;
        }

        VkCommandBuffer cmd_buf = frames->cmd_bufs[frames->frame_index];
        vk_command_buffer_begin(cmd_buf, 0);
        surface_vk_setup_viewport(cmd_buf, vc->gfx.ds, ww, wh);
        surface_vk_render_texture(vc->gfx.vk_device, &vc->gfx.vk_swapchain, &vc->gfx.vk_frames, vc->gfx.vks, vc->gfx.ds);

        // Wait the swapchain image has been acquired before writing color onto it
        VkSemaphore wait_semaphores[] = {image_available_semaphore};
        VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

        // Signal this semaphore once the commands have finished execution
        VkSemaphore signal_semaphores[] = {render_finished_semaphore};

        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = wait_semaphores,
            .pWaitDstStageMask = wait_stages,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd_buf,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = signal_semaphores,
        };

        VK_CHECK(vkQueueSubmit(device.graphics_queue, 1, &submit_info, fence));

        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            // Wait for commands to finish before presenting the result
            .pWaitSemaphores = signal_semaphores,
            .swapchainCount = 1,
            .pSwapchains = &swapchain->handle,
            .pImageIndices = &image_index,
        };

        VkResult present_res = vkQueuePresentKHR(device.present_queue, &present_info);
        if (present_res == VK_ERROR_OUT_OF_DATE_KHR)
        {
            vk_swapchain_recreate(device, swapchain, 0, 0);

            for (uint32_t i = 0; i < swapchain->image_count; ++i)
            {
                vkDestroyFramebuffer(device.handle, frames->framebuffers[i], NULL);
                frames->framebuffers[i] = vk_create_framebuffer(device.handle, shader->texture_blit_render_pass, swapchain->views[i], swapchain->extent.width, swapchain->extent.height);
            }

            frames->frame_index = 0;
            return;
        }

        VK_CHECK(vkQueueWaitIdle(device.present_queue));

        vkDestroySemaphore(device.handle, image_available_semaphore, NULL);
        vkDestroySemaphore(device.handle, render_finished_semaphore, NULL);
        vkDestroyFence(device.handle, fence, NULL);

        vc->gfx.scale_x = (double)ww / surface_width(vc->gfx.ds);
        vc->gfx.scale_y = (double)wh / surface_height(vc->gfx.ds);
    }

    // TODO: present here?
    //glFlush();
    graphic_hw_gl_flushed(vc->gfx.dcl.con);
}

void gd_vk_update(DisplayChangeListener *dcl,
                   int x, int y, int w, int h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (!vc->gfx.vks || !vc->gfx.ds) {
        return;
    }

    surface_vk_update_texture(vc->gfx.vk_device, vc->gfx.ds, x, y, w, h);
    vc->gfx.glupdates++;
}

void gd_vk_refresh(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.dcl.update_interval = gd_monitor_update_interval(
            vc->window ? vc->window : vc->gfx.drawing_area);

    if (vc->gfx.vk_device.handle == VK_NULL_HANDLE) {
        gd_vk_init(vc);
        if (vc->gfx.vk_device.handle == VK_NULL_HANDLE) {
            return;
        }
        vc->gfx.vks = qemu_vk_init_shader(vc->gfx.vk_device, vc->gfx.vk_swapchain.format);
        if (vc->gfx.ds) {
            surface_vk_create_texture(vc->gfx.vk_device, vc->gfx.ds);
            vc->gfx.vk_frames = vk_create_frames(vc->gfx.vk_device, vc->gfx.vk_swapchain, vc->gfx.vks);
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

    surface_vk_destroy_texture(vc->gfx.vk_device.handle, vc->gfx.ds);
    vc->gfx.ds = surface;
    if (vc->gfx.vks) {
        surface_vk_create_texture(vc->gfx.vk_device, vc->gfx.ds);
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
    vk_fb_setup_for_tex(vc->gfx.vk_device.handle, &vc->gfx.guest_vk_fb, texture);
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
        return; // Scanout disabled
    }

    if (!vc->gfx.guest_vk_fb.framebuffers) {
        return; // Nothing to blit
    }

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    ww = gdk_window_get_width(window);
    wh = gdk_window_get_height(window);
    vk_fb_setup_default(vc->gfx.vk_device, &vc->gfx.vk_swapchain, &vc->gfx.vk_frames, vc->gfx.vks->texture_blit_render_pass, ww, wh);
    // TODO acquire next image?

    if (vc->gfx.cursor_fb.texture) {
        //vk_texture_blit(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.guest_fb,
        //                 vc->gfx.y0_top);
        //vk_texture_blend(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.cursor_fb,
        //                  vc->gfx.y0_top,
        //                  vc->gfx.cursor_x, vc->gfx.cursor_y,
        //                  vc->gfx.scale_x, vc->gfx.scale_y);
    } else {
       // vk_fb_blit(&vc->gfx.win_fb, &vc->gfx.guest_fb, !vc->gfx.y0_top);
    }

    // TODO: present
    //eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
}

void gtk_vk_init(void)
{
    display_vulkan = 1;
}
