#ifndef VK_CONTEXT_H
#define VK_CONTEXT_H

#include "ui/vulkan-helpers.h"

QEMUVulkanContext qemu_vk_create_context(void);
void qemu_vk_destroy_context(QEMUVulkanContext ctx);

#endif /* VK_CONTEXT_H */
