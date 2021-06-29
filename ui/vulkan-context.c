#include "qemu/osdep.h"
#include "ui/vulkan-context.h"

QEMUVulkanContext qemu_vk_create_context(DisplayChangeListener *dcl,
                                         QEMUGLParams *params)
{
   return vk_create_instance();
}

void qemu_vk_destroy_context(DisplayChangeListener *dcl, QEMUVulkanContext ctx)
{
    vkDestroyInstance(ctx, NULL);
}

int qemu_vk_make_context_current(DisplayChangeListener *dcl,
                                  QEMUVulkanContext ctx)
{
    // TODO: is this needed? no?
   //return eglMakeCurrent(qemu_egl_display,
   //                      EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
   return 0;
}
