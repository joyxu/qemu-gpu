#include "qemu/osdep.h"
#include "ui/vulkan-context.h"
#include "ui/vulkan-helpers.h"

QEMUVulkanContext qemu_vk_create_context(void)
{
   return vk_create_context();
}

void qemu_vk_destroy_context(QEMUVulkanContext ctx)
{
    vkDestroyInstance(ctx.instance, NULL);
}
