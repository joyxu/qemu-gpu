#ifndef VULKAN_HELPERS_H
#define VULKAN_HELPERS_H

#include <X11/Xlib.h>
#include <wayland-client.h>
#include <vulkan/vulkan_core.h>
#include <stdbool.h>

typedef struct QEMUVkQueueFamilyIndices {
    int32_t graphics;
    int32_t present;
} QEMUVkQueueFamilyIndices;


typedef struct QEMUVkPhysicalDevice {
    VkPhysicalDevice handle;
    QEMUVkQueueFamilyIndices queue_family_indices;
    VkPhysicalDeviceMemoryProperties memory_properties;
} QEMUVkPhysicalDevice;


typedef struct QEMUVkDevice {
    QEMUVkPhysicalDevice physical_device;
    VkDevice handle;
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkCommandPool command_pool;
    VkCommandBuffer general_command_buffer;
} QEMUVkDevice;

typedef struct QEMUVkSwapchain {
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *images;
    VkImageView *views;
    VkFramebuffer *framebuffers;
} QEMUVkSwapchain;

typedef struct QEMUVulkanContext {
    VkInstance instance;
    QEMUVkPhysicalDevice physical_device;
    QEMUVkDevice device;
    VkSurfaceKHR surface;
    QEMUVkSwapchain swapchain;
} QEMUVulkanContext;

typedef struct vulkan_texture
{
    int width;
    int height;
    VkImage image;
    VkImageView view;
    // This is going to be false in case of a swapchain image
    // In that case, both image and view are owned by the swapchain
    bool delete_image;
} vulkan_texture;

typedef struct vulkan_fb
{
    vulkan_texture texture;
    uint32_t framebuffer_count;
    VkFramebuffer *framebuffers;
} vulkan_fb;

void vk_fb_setup_for_tex(VkDevice device, vulkan_fb *fb, vulkan_texture texture);
void vk_fb_setup_new_tex(VkDevice device, vulkan_fb *fb, int width, int height);
void vk_fb_setup_default(VkDevice device, QEMUVkSwapchain swapchain, VkRenderPass render_pass, vulkan_fb *fb, int width, int height);
void vk_fb_destroy(VkDevice device, vulkan_fb *fb);

VkInstance vk_create_instance(void);
QEMUVkPhysicalDevice vk_get_physical_device(VkInstance i, VkSurfaceKHR s);
QEMUVkDevice vk_create_device(VkInstance i, QEMUVkPhysicalDevice pd);
QEMUVkSwapchain vk_create_swapchain(QEMUVkPhysicalDevice pd, VkDevice d, VkSurfaceKHR s);

QEMUVkDevice vk_init(void);

VkSurfaceKHR qemu_vk_init_surface_x11(VkInstance instance, Display *dpy, Window w);

int qemu_vk_init_dpy_wayland(struct wl_display *dpy, struct wl_surface *s);
int qemu_vk_init_dpy_x11(Display *dpy, Window w);


QEMUVulkanContext vk_create_context(void);
void vk_destroy_context(QEMUVulkanContext ctx);

#endif // VULKAN_HELPERS_H
