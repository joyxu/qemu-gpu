#ifndef VK_CONTEXT_H
#define VK_CONTEXT_H

#include "ui/console.h"
#include "ui/vulkan-helpers.h"

QEMUVulkanContext qemu_vk_create_context(DisplayChangeListener *dcl, QEMUGLParams *params);
void qemu_vk_destroy_context(DisplayChangeListener *dcl, QEMUVulkanContext ctx);
int qemu_vk_make_context_current(DisplayChangeListener *dcl, QEMUVulkanContext ctx);

#endif /* VK_CONTEXT_H */
