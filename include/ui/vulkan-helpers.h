#ifndef VULKAN_HELPERS_H
#define VULKAN_HELPERS_H

#include <X11/Xlib.h>
#include <wayland-client.h>
#include <vulkan/vulkan_core.h>
#include <stdbool.h>

typedef struct QEMUVkQueueFamilyIndices
{
    int32_t graphics;
    int32_t present;
} QEMUVkQueueFamilyIndices;

typedef struct QEMUVkPhysicalDevice
{
    VkSurfaceKHR surface;
    VkPhysicalDevice handle;
    QEMUVkQueueFamilyIndices queue_family_indices;
    VkPhysicalDeviceMemoryProperties memory_properties;
} QEMUVkPhysicalDevice;

typedef struct QEMUVkDevice
{
    QEMUVkPhysicalDevice physical_device;
    VkDevice handle;
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkCommandPool command_pool;
    VkCommandBuffer general_command_buffer;
} QEMUVkDevice;

typedef struct QEMUVkSwapchain
{
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *images;
    VkImageView *views;
} QEMUVkSwapchain;

typedef struct QEMUVkBuffer
{
    VkBuffer handle;
    VkDeviceMemory memory;
} QEMUVkBuffer;

typedef struct QEMUVulkanContext
{
    VkInstance instance;
    QEMUVkPhysicalDevice physical_device;
    QEMUVkDevice device;
    VkSurfaceKHR surface;
    QEMUVkSwapchain swapchain;
} QEMUVulkanContext;

typedef struct QEMUVkFrames
{
    VkDescriptorPool desc_pool;
    uint32_t frame_count;
    uint32_t frame_index;
    VkFramebuffer *framebuffers;
    VkCommandBuffer *cmd_bufs;
    VkDescriptorSet *desc_sets;
} QEMUVkFrames;

typedef struct QEMUVulkanShader QEMUVulkanShader;

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
void vk_fb_setup_default(QEMUVkDevice device, QEMUVkSwapchain *swapchain, QEMUVkFrames *frames, VkRenderPass render_pass, int width, int height);
void vk_fb_destroy(VkDevice device, vulkan_fb *fb);

VkInstance vk_create_instance(void);
QEMUVkPhysicalDevice vk_get_physical_device(VkInstance i, VkSurfaceKHR s);
QEMUVkDevice vk_create_device(VkInstance i, QEMUVkPhysicalDevice pd);
QEMUVkSwapchain vk_create_swapchain(QEMUVkDevice d, VkSwapchainKHR old_swapchain, uint32_t width, uint32_t height);
void vk_swapchain_recreate(QEMUVkDevice device, QEMUVkSwapchain *swapchain, uint32_t width, uint32_t height);

VkFramebuffer vk_create_framebuffer(VkDevice device, VkRenderPass render_pass, VkImageView view, uint32_t width, uint32_t height);

/// Creates `swapchain.image_count` frames
QEMUVkFrames vk_create_frames(QEMUVkDevice device, QEMUVkSwapchain swapchain, QEMUVulkanShader *shader);

QEMUVkDevice vk_init(void);

VkSurfaceKHR vk_create_x11_surface(VkInstance instance, Display *dpy, Window w);
VkSurfaceKHR vk_create_wayland_surface(VkInstance instance, struct wl_display *dpy, struct wl_surface *s);

QEMUVulkanContext vk_create_context(void);
void vk_destroy_context(QEMUVulkanContext ctx);

void vk_command_buffer_begin(VkCommandBuffer command_buffer, VkCommandBufferUsageFlags flags);

uint32_t get_memory_type_index(QEMUVkPhysicalDevice physical_device,
                               uint32_t memory_type_filter,
                               VkMemoryPropertyFlags memory_property_flags);

QEMUVkBuffer vk_create_buffer(QEMUVkDevice device,
                              VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags memory_properties);

void vk_destroy_buffer(VkDevice device, QEMUVkBuffer buffer);

VkSemaphore vk_create_semaphore(VkDevice device);
VkFence vk_create_fence(VkDevice device);

#endif // VULKAN_HELPERS_H
