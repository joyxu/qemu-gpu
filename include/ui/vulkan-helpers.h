#ifndef VULKAN_HELPERS_H
#define VULKAN_HELPERS_H

#include <vulkan/vulkan_core.h>
#include <stdbool.h>

typedef struct vulkan_texture
{
    int width;
    int height;
    VkImage image;
    VkImageView view;
    // This is going to be false in case of a swapchain image
    bool delete_image;
} vulkan_texture;

typedef struct vulkan_fb
{
    vulkan_texture texture;
    VkFramebuffer framebuffer;
} vulkan_fb;

void vk_fb_setup_for_tex(VkDevice device, vulkan_fb *fb, vulkan_texture texture);
void vk_fb_setup_new_tex(VkDevice device, vulkan_fb *fb, int width, int height);
void vk_fb_destroy(VkDevice device, vulkan_fb *fb);

VkDevice vk_init(void);

#endif // VULKAN_HELPERS_H
