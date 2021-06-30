#ifndef VK_CONTEXT_H
#define VK_CONTEXT_H

#include <vulkan/vulkan_core.h>
typedef struct QEMUVulkanContext {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
} QEMUVulkanContext;

QEMUVulkanContext qemu_vk_create_context(void);
void qemu_vk_destroy_context(QEMUVulkanContext ctx);

#endif /* VK_CONTEXT_H */
