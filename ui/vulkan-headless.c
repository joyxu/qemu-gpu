
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "ui/vulkan-helpers.h"
#include "ui/vulkan-shader.h"
#include "ui/egl-context.h"

typedef struct vulkan_dpy
{
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    VkDevice device;
    QEMUVulkanShader *vks;
    vulkan_fb guest_fb;
    vulkan_fb cursor_fb;
    vulkan_fb blit_fb;
    bool y_0_top;
    uint32_t pos_x;
    uint32_t pos_y;
} vulkan_dpy;

/* ------------------------------------------------------------------ */

static void vk_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static void vk_gfx_update(DisplayChangeListener *dcl,
                          int x, int y, int w, int h)
{
    // TODO: Recreate swapchain?
}

static void vk_gfx_switch(DisplayChangeListener *dcl,
                          struct DisplaySurface *new_surface)
{
    vulkan_dpy *vdpy = container_of(dcl, vulkan_dpy, dcl);

    vdpy->ds = new_surface;
}

static void vk_scanout_disable(DisplayChangeListener *dcl)
{
    vulkan_dpy *vdpy = container_of(dcl, vulkan_dpy, dcl);

    vk_fb_destroy(vdpy->device, &vdpy->guest_fb);
    vk_fb_destroy(vdpy->device, &vdpy->blit_fb);
}

static void vk_scanout_texture(DisplayChangeListener *dcl,
                               uint32_t backing_id,
                               bool backing_y_0_top,
                               uint32_t backing_width,
                               uint32_t backing_height,
                               uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
    vulkan_dpy *vdpy = container_of(dcl, vulkan_dpy, dcl);

    vdpy->y_0_top = backing_y_0_top;

    // TODO should this be a parameter?
    vulkan_texture texture = {};

    /* source framebuffer */
    vk_fb_setup_for_tex(vdpy->device, &vdpy->guest_fb, texture);

    /* dest framebuffer */
    // TODO: is this the swapchain?
    if (vdpy->blit_fb.texture.width != backing_width ||
        vdpy->blit_fb.texture.height != backing_height)
    {
        vk_fb_destroy(vdpy->device, &vdpy->blit_fb);
        vk_fb_setup_new_tex(vdpy->device, &vdpy->blit_fb, backing_width, backing_height);
    }
}
#if 0

static void egl_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    egl_dmabuf_import_texture(dmabuf);
    if (!dmabuf->texture) {
        return;
    }

    egl_scanout_texture(dcl, dmabuf->texture,
                        false, dmabuf->width, dmabuf->height,
                        0, 0, dmabuf->width, dmabuf->height);
}

static void egl_cursor_dmabuf(DisplayChangeListener *dcl,
                              QemuDmaBuf *dmabuf, bool have_hot,
                              uint32_t hot_x, uint32_t hot_y)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    if (dmabuf) {
        egl_dmabuf_import_texture(dmabuf);
        if (!dmabuf->texture) {
            return;
        }
        egl_fb_setup_for_tex(&edpy->cursor_fb, dmabuf->width, dmabuf->height,
                             dmabuf->texture, false);
    } else {
        egl_fb_destroy(&edpy->cursor_fb);
    }
}

static void egl_cursor_position(DisplayChangeListener *dcl,
                                uint32_t pos_x, uint32_t pos_y)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    edpy->pos_x = pos_x;
    edpy->pos_y = pos_y;
}

static void egl_release_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    egl_dmabuf_release_texture(dmabuf);
}

static void egl_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    if (!edpy->guest_fb.texture || !edpy->ds) {
        return;
    }
    assert(surface_format(edpy->ds) == PIXMAN_x8r8g8b8);

    if (edpy->cursor_fb.texture) {
        /* have cursor -> render using textures */
        egl_texture_blit(edpy->gls, &edpy->blit_fb, &edpy->guest_fb,
                         !edpy->y_0_top);
        egl_texture_blend(edpy->gls, &edpy->blit_fb, &edpy->cursor_fb,
                          !edpy->y_0_top, edpy->pos_x, edpy->pos_y,
                          1.0, 1.0);
    } else {
        /* no cursor -> use simple framebuffer blit */
        egl_fb_blit(&edpy->blit_fb, &edpy->guest_fb, edpy->y_0_top);
    }

    egl_fb_read(edpy->ds, &edpy->blit_fb);
    dpy_gfx_update(edpy->dcl.con, x, y, w, h);
}

#endif // 0
static const DisplayChangeListenerOps vulkan_ops = {
    .dpy_name = "vulkan-headless",
    .dpy_refresh = vk_refresh,
    .dpy_gfx_update = vk_gfx_update,
    .dpy_gfx_switch = vk_gfx_switch,

// Only needed by guest GL?
    .dpy_gl_ctx_create = NULL,
    .dpy_gl_ctx_destroy = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,

    .dpy_gl_scanout_disable  = vk_scanout_disable,
    .dpy_gl_scanout_texture  = vk_scanout_texture,
    //.dpy_gl_scanout_dmabuf   = egl_scanout_dmabuf,
    //.dpy_gl_cursor_dmabuf    = egl_cursor_dmabuf,
    //.dpy_gl_cursor_position  = egl_cursor_position,
    //.dpy_gl_release_dmabuf   = egl_release_dmabuf,
    //.dpy_gl_update           = egl_scanout_flush,
};

static void early_vk_headless_init(DisplayOptions *opts)
{
    display_vulkan = 1;
}

static void vk_headless_init(DisplayState *ds, DisplayOptions *opts)
{
    QEMUVkDevice device;
    QemuConsole *con;
    vulkan_dpy *vdpy;
    int idx;

    // TODO accept an index for selecting physical device
    device = vk_init();

    /*
    DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAYGL_MODE_ON;

    if (egl_rendernode_init(opts->u.egl_headless.rendernode, mode) < 0) {
        error_report("egl: render node init failed");
        exit(1);
    }
    */

    for (idx = 0;; idx++)
    {
        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con))
        {
            break;
        }

        vdpy = g_new0(vulkan_dpy, 1);
        vdpy->dcl.con = con;
        vdpy->dcl.ops = &vulkan_ops;
        vdpy->device = device.handle; // TODO different device for each display?
        vdpy->vks = qemu_vk_init_shader(device, VK_FORMAT_B8G8R8A8_UNORM);
        register_displaychangelistener(&vdpy->dcl);
    }
}

static QemuDisplay qemu_display_vulkan = {
    .type = DISPLAY_TYPE_VULKAN_HEADLESS,
    .early_init = early_vk_headless_init,
    .init = vk_headless_init,
};

static void register_vulkan(void)
{
    qemu_display_register(&qemu_display_vulkan);
}

type_init(register_vulkan);
