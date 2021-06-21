#ifndef VULKAN_HELPERS_H
#define VULKAN_HELPERS_H

#include <vulkan/vulkan.h>
#include <stdbool.h>

typedef struct vulkan_fb {
    int width;
    int height;
    VkImage texture;
    VkFramebuffer framebuffer;
    bool delete_texture;
} vulkan_fb;

void vulkan_init(void);

#endif // VULKAN_HELPERS_H
