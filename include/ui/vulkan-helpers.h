#ifndef VULKAN_HELPERS_H
#define VULKAN_HELPERS_H

#include <X11/Xlib.h>
#include <wayland-client.h>
#include <vulkan/vulkan_core.h>
#include <stdbool.h>

extern VkDevice device;

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

void vk_fb_setup_for_tex(vulkan_fb *fb, vulkan_texture texture);
void vk_fb_setup_new_tex(vulkan_fb *fb, int width, int height);
void vk_fb_destroy(vulkan_fb *fb);

VkInstance vk_create_instance(void);
VkDevice vk_init(void);

VkSurfaceKHR qemu_vk_init_surface_x11(VkInstance instance, Display *dpy, Window w);

int qemu_vk_init_dpy_wayland(struct wl_display *dpy, struct wl_surface *s);
int qemu_vk_init_dpy_x11(Display *dpy, Window w);


VkInstance qemu_vk_create_instance(void);
VkDevice qemu_vk_create_device(VkInstance instance);

#endif // VULKAN_HELPERS_H
