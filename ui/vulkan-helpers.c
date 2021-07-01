/*
 * Copyright (C) 2021 Collabora Ltd.
 *
 * Authors:
 *    Antonio Caggiano <antonio.caggiano@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/vulkan-helpers.h"

#include <glib.h>
#include <vulkan/vulkan_wayland.h>
#include <vulkan/vulkan_xlib.h>

#define VK_CHECK(res) g_assert(res == VK_SUCCESS)

// TODO where do we destroy these?
//VkInstance instance;
//VkDevice device;

/* -- */

static void vk_texture_destroy(VkDevice device, vulkan_texture *texture)
{
    if (texture->delete_image)
    {
        if (texture->image != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, texture->image, NULL);
            texture->image = VK_NULL_HANDLE;
        }

        if (texture->view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, texture->view, NULL);
            texture->view = VK_NULL_HANDLE;
        }

        texture->delete_image = false;
    }

    texture->width = 0;
    texture->height = 0;
}

void vk_fb_destroy(VkDevice device, vulkan_fb *fb)
{
    if (!fb->framebuffers)
    {
        return;
    }

    vk_texture_destroy(device, &fb->texture);

    for (uint32_t i = 0; i < fb->framebuffer_count; ++i) {
        vkDestroyFramebuffer(device, fb->framebuffers[i], NULL);
        fb->framebuffers[i] = VK_NULL_HANDLE;
    }

    g_free(fb->framebuffers);
    fb->framebuffer_count = 0;
}

// Default framebuffer is the one using the swapchain images
void vk_fb_setup_default(VkDevice device, QEMUVkSwapchain swapchain, VkRenderPass render_pass, vulkan_fb *fb, int width, int height)
{
    g_assert(width == swapchain.extent.width && height == swapchain.extent.height);

    if (fb->framebuffers != NULL) {
        return; // TODO: already setup?
    }

    fb->texture = (vulkan_texture) {
        .width = width,
        .height = height,
        .image = VK_NULL_HANDLE,
        .view = VK_NULL_HANDLE,
        // Do not delete image when destroying this framebuffer as it is a managed swapchain image
        .delete_image = false,
    };

    fb->framebuffer_count = swapchain.image_count;
    fb->framebuffers = g_new(VkFramebuffer, fb->framebuffer_count);

    for (uint32_t i = 0; i < swapchain.image_count; ++i) {
        VkImageView attachments[] = { swapchain.views[i] };
        VkFramebufferCreateInfo framebuffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 1,
            .pAttachments = attachments,
            .width = width,
            .height = height,
            .layers = 1,
        };

        VK_CHECK(vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &fb->framebuffers[i]));
    }
}

void vk_fb_setup_for_tex(VkDevice device, vulkan_fb *fb, vulkan_texture texture)
{
    // Destroy previous framebuffer and then set the new one
    vk_fb_destroy(device, fb);

    fb->texture = texture;

    fb->framebuffer_count = 1;
    fb->framebuffers = g_new(VkFramebuffer, 1);

    VkFramebufferCreateInfo framebuffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = VK_NULL_HANDLE,
        .attachmentCount = 1,
        .pAttachments = &fb->texture.view,
        .width = fb->texture.width,
        .height = fb->texture.height,
        .layers = 1,
    };

    VK_CHECK(vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &fb->framebuffers[0]));
}

void vk_fb_setup_new_tex(VkDevice device, vulkan_fb *fb, int width, int height)
{
    vulkan_texture texture = {
        .width = width,
        .height = height,
        .delete_image = true,
        .image = VK_NULL_HANDLE,
        .view = VK_NULL_HANDLE,
    };

    // TODO find the right format
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, // TODO figure out usage of this fb
    };

    VK_CHECK(vkCreateImage(device, &image_create_info, NULL, &texture.image));

    // TODO: Bind memory

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        }};

    VK_CHECK(vkCreateImageView(device, &image_view_create_info, NULL, &texture.view));

    vk_fb_setup_for_tex(device, fb, texture);
}

/* -- */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message_callback(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT object_type,
    uint64_t object,
    size_t location,
    int32_t message_code,
    const char *layer_prefix,
    const char *message,
    void *user_data)
{
    g_printerr("[Vulkan validation]: %s - %s\n", layer_prefix, message);
    return VK_FALSE;
}

VkInstance vk_create_instance(void)
{
    VkInstance instance = VK_NULL_HANDLE;

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Qemu",
        .pEngineName = "Qemu",
        .apiVersion = VK_API_VERSION_1_0,
    };

    const char* extension_names[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
    };

    VkInstanceCreateInfo instance_create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = 3, // exclude debug report for the moment
        .ppEnabledExtensionNames = extension_names,
    };

    const char *validation_layers[] = { "VK_LAYER_KHRONOS_validation"};
    uint32_t layer_count = 1;

    // Check layers availability
    uint32_t instance_layer_count;
    VK_CHECK(vkEnumerateInstanceLayerProperties(&instance_layer_count, NULL));

    VkLayerProperties *instance_layers = g_new(VkLayerProperties, instance_layer_count);
    VK_CHECK(vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layers));

    bool layers_available = true;

    for (uint32_t i = 0; i < layer_count; ++i)
    {
        bool current_layer_available = false;
        for (uint32_t j = 0; j < instance_layer_count; ++j)
        {
            if (strcmp(instance_layers[j].layerName, validation_layers[i]) == 0)
            {
                current_layer_available = true;
                break;
            }
        }
        if (!current_layer_available)
        {
            layers_available = false;
            break;
        }
    }

    // TODO: remove and enable validation layers only on debug mode
    g_assert(layers_available);

    if (layers_available)
    {
        instance_create_info.ppEnabledLayerNames = validation_layers;
        instance_create_info.enabledLayerCount = layer_count;
        // Extension name to be enabled is last one, so we just increase the count here
        instance_create_info.enabledExtensionCount += 1;
    }

    VK_CHECK(vkCreateInstance(&instance_create_info, NULL, &instance));

    g_free(instance_layers);

    // Setup debug report
    VkDebugReportCallbackEXT debug_report_callback = VK_NULL_HANDLE;

    if (layers_available)
    {
        VkDebugReportCallbackCreateInfoEXT debug_report_create_info = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
            .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT,
            .pfnCallback = (PFN_vkDebugReportCallbackEXT)debug_message_callback,
        };

        // We have to explicitly load this function
        PFN_vkCreateDebugReportCallbackEXT vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)(vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT"));
        g_assert(vkCreateDebugReportCallbackEXT);
        VK_CHECK(vkCreateDebugReportCallbackEXT(instance, &debug_report_create_info, NULL, &debug_report_callback));
    }

    return instance;
}

/// Finds the graphics and present family indices
static QEMUVkQueueFamilyIndices get_queue_family_indices(VkPhysicalDevice physical_device, VkSurfaceKHR surface)
{
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);

    VkQueueFamilyProperties *queue_families = g_new(VkQueueFamilyProperties, queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families);

    QEMUVkQueueFamilyIndices indices = {
        .graphics = -1,
        .present = -1,
    };

    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics = (int32_t)i;
        }

        VkBool32 present_support = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &present_support);
        if (present_support) {
            indices.present = (int32_t)i;
        }

        if (indices.graphics >= 0 && indices.present >= 0) {
            break;
        }
    }

    g_free(queue_families);
    return indices;
}

bool support_graphics_and_present(QEMUVkQueueFamilyIndices indices)
{
    return indices.graphics >= 0 && indices.present >= 0;
}

QEMUVkPhysicalDevice vk_get_physical_device(VkInstance instance, VkSurfaceKHR surface)
{
    uint32_t device_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, NULL));

    VkPhysicalDevice *physical_devices = g_new(VkPhysicalDevice, device_count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, physical_devices));

    QEMUVkPhysicalDevice physical_device = {
        .handle = VK_NULL_HANDLE,
        .queue_family_indices = {
            .graphics = -1,
            .present = -1,
        }
    };

    for (uint32_t i = 0; i < device_count; ++i) {
        physical_device.queue_family_indices = get_queue_family_indices(physical_devices[i], surface);
        if (support_graphics_and_present(physical_device.queue_family_indices)) {
            physical_device.handle = physical_devices[i];
            break;
        }
    }

    vkGetPhysicalDeviceMemoryProperties(physical_device.handle, &physical_device.memory_properties);

    g_free(physical_devices);
    g_assert(physical_device.handle != VK_NULL_HANDLE);
    return physical_device;
}

QEMUVkDevice vk_create_device(VkInstance instance, QEMUVkPhysicalDevice physical_device)
{
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical_device.handle, &device_properties);
    g_print("Bringing up Vulkan on %s\n", device_properties.deviceName);
    
    const float default_queue_priority = 1.0f;

    uint32_t queue_count =
        physical_device.queue_family_indices.graphics == physical_device.queue_family_indices.present ?
        1 : 2;

    VkDeviceQueueCreateInfo *queue_create_infos = g_new(VkDeviceQueueCreateInfo, queue_count);

    queue_create_infos[0] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = physical_device.queue_family_indices.graphics,
        .queueCount = 1,
        .pQueuePriorities = &default_queue_priority
    };

    if (queue_count > 1) {
        queue_create_infos[1] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = physical_device.queue_family_indices.present,
            .queueCount = 1,
            .pQueuePriorities = &default_queue_priority
        };
    }

    const char** extension_names[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = queue_count,
        .pQueueCreateInfos = queue_create_infos,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extension_names,
    };

    QEMUVkDevice device = {
        .physical_device = physical_device,
        .handle = VK_NULL_HANDLE,
        .graphics_queue = VK_NULL_HANDLE,
        .present_queue = VK_NULL_HANDLE,
    };

    VK_CHECK(vkCreateDevice(physical_device.handle, &device_create_info, NULL, &device.handle));

    g_free(queue_create_infos);
    g_assert(device.handle != VK_NULL_HANDLE);

    vkGetDeviceQueue(device.handle, physical_device.queue_family_indices.graphics, 0, &device.graphics_queue);
    vkGetDeviceQueue(device.handle, physical_device.queue_family_indices.present, 0, &device.present_queue);

    // Command pool
    VkCommandPoolCreateInfo command_pool_create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = physical_device.queue_family_indices.graphics,
    };

    VK_CHECK(vkCreateCommandPool(device.handle, &command_pool_create_info, NULL, &device.command_pool));

    // General command buffer
    VkCommandBufferAllocateInfo command_buffer_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = device.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VK_CHECK(vkAllocateCommandBuffers(device.handle, &command_buffer_alloc_info, &device.general_command_buffer));

    return device;
}

// TODO: What is this function used for?
QEMUVkDevice vk_init(void)
{
    VkInstance instance = vk_create_instance();
    QEMUVkPhysicalDevice physical_device = vk_get_physical_device(instance, VK_NULL_HANDLE);
    return vk_create_device(instance, physical_device);
}

static VkSurfaceKHR vk_create_wayland_surface(VkInstance instance, struct wl_display* dpy, struct wl_surface* s)
{
    VkWaylandSurfaceCreateInfoKHR surface_create_info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .flags = 0,
        .display = dpy,
        .surface = s,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VK_CHECK(vkCreateWaylandSurfaceKHR(instance, &surface_create_info, NULL, &surface));
    return surface;
}

VkSurfaceKHR qemu_vk_init_surface_x11(VkInstance instance, Display* dpy, Window w)
{
    VkXlibSurfaceCreateInfoKHR surface_create_info = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .flags = 0,
        .dpy = dpy,
        .window = w,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VK_CHECK(vkCreateXlibSurfaceKHR(instance, &surface_create_info, NULL, &surface));
    return surface;
}

static VkSurfaceKHR vk_create_x11_surface(VkInstance instance, Display *dpy, Window w)
{
    return qemu_vk_init_surface_x11(instance, dpy, w);
}

static int qemu_vk_init_dpy(VkDisplayKHR dpy,
                             //EGLenum platform,
                             int platform,
                             VkDisplayModeKHR mode)
{
    /*
    static const EGLint conf_att_core[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,   5,
        EGL_GREEN_SIZE, 5,
        EGL_BLUE_SIZE,  5,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    static const EGLint conf_att_gles[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   5,
        EGL_GREEN_SIZE, 5,
        EGL_BLUE_SIZE,  5,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLint major, minor;
    EGLBoolean b;
    EGLint n;
    bool gles = (mode == DISPLAYGL_MODE_ES);

    qemu_egl_display = qemu_egl_get_display(dpy, platform);
    if (qemu_egl_display == EGL_NO_DISPLAY) {
        error_report("egl: eglGetDisplay failed");
        return -1;
    }

    b = eglInitialize(qemu_egl_display, &major, &minor);
    if (b == EGL_FALSE) {
        error_report("egl: eglInitialize failed");
        return -1;
    }

    b = eglBindAPI(gles ?  EGL_OPENGL_ES_API : EGL_OPENGL_API);
    if (b == EGL_FALSE) {
        error_report("egl: eglBindAPI failed (%s mode)",
                     gles ? "gles" : "core");
        return -1;
    }

    b = eglChooseConfig(qemu_egl_display,
                        gles ? conf_att_gles : conf_att_core,
                        &qemu_egl_config, 1, &n);
    if (b == EGL_FALSE || n != 1) {
        error_report("egl: eglChooseConfig failed (%s mode)",
                     gles ? "gles" : "core");
        return -1;
    }

    qemu_egl_mode = gles ? DISPLAYGL_MODE_ES : DISPLAYGL_MODE_CORE;
    return 0;
    */
}

/// Initializes Vulkan for a wayland display
int qemu_vk_init_dpy_wayland(struct wl_display *dpy, struct wl_surface *s)
{
    VkInstance instance = vk_create_instance();
    vk_create_wayland_surface(instance, dpy, s);
}

/// Initializes Vulkan for an X11 display
int qemu_vk_init_dpy_x11(Display *dpy, Window w)
{
    VkInstance instance = vk_create_instance();
    vk_create_x11_surface(instance, dpy, w);
}

static VkExtent2D get_swapchain_extent(VkSurfaceCapabilitiesKHR *capabilities) {
    g_assert(capabilities->currentExtent.width != UINT32_MAX);
    return capabilities->currentExtent;
}

static VkSurfaceFormatKHR get_swapchain_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {

    uint32_t format_count;
    VkSurfaceFormatKHR format = {};
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, NULL));

    VkSurfaceFormatKHR *formats = g_new(VkSurfaceFormatKHR, format_count);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats));

    for (uint32_t i = 0; i < format_count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format = formats[i];
            break;
        }
    }

    g_free(formats);
    g_assert(format.format != VK_FORMAT_UNDEFINED);
    return format;
}

static VkPresentModeKHR get_swapchain_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
    uint32_t present_count;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_count, NULL));

    VkPresentModeKHR *present_modes = g_new(VkPresentModeKHR, present_count);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_count, present_modes));

    for (uint32_t i = 0; i < present_count; ++i) {
        if (present_modes[i] == VK_PRESENT_MODE_FIFO_KHR) {
            present_mode = present_modes[i];
            break;
        }
    }

    g_free(present_modes);
    return present_mode;
}

QEMUVkSwapchain vk_create_swapchain(QEMUVkPhysicalDevice physical_device, VkDevice device, VkSurfaceKHR surface) {
    VkSurfaceCapabilitiesKHR capabilities = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device.handle, surface, &capabilities);

    VkExtent2D extent = get_swapchain_extent(&capabilities);
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkSurfaceFormatKHR format = get_swapchain_format(physical_device.handle, surface);
    VkPresentModeKHR present_mode = get_swapchain_present_mode(physical_device.handle, surface);

    VkSharingMode image_sharing_mode =
        physical_device.queue_family_indices.graphics == physical_device.queue_family_indices.present
        ? VK_SHARING_MODE_EXCLUSIVE
        : VK_SHARING_MODE_CONCURRENT;

    VkSwapchainCreateInfoKHR swapchain_create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = image_sharing_mode,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        // TODO old swapchain?
    };

    QEMUVkSwapchain swapchain = {
        .handle = VK_NULL_HANDLE,
        .format = format.format,
        .extent = extent,
        .image_count = 0,
        .images = NULL,
    };

    VK_CHECK(vkCreateSwapchainKHR(device, &swapchain_create_info, NULL, &swapchain.handle));
    g_assert(swapchain.handle != VK_NULL_HANDLE);

    vkGetSwapchainImagesKHR(device, swapchain.handle, &swapchain.image_count, NULL);
    g_assert(swapchain.image_count > 0);
    swapchain.images = g_new(VkImage, swapchain.image_count);
    vkGetSwapchainImagesKHR(device, swapchain.handle, &swapchain.image_count, swapchain.images);

    swapchain.views = g_new(VkImageView, swapchain.image_count);

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = VK_NULL_HANDLE,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = swapchain.format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        }
    };

    for (uint32_t i = 0; i < swapchain.image_count; ++i) {
        image_view_create_info.image = swapchain.images[i];
        VK_CHECK(vkCreateImageView(device, &image_view_create_info, NULL, &swapchain.views[i]));
    }

    return swapchain;
}

QEMUVulkanContext vk_create_context(void)
{
    QEMUVulkanContext ctx = {};

    ctx.instance = vk_create_instance();
    ctx.physical_device = vk_get_physical_device(ctx.instance, VK_NULL_HANDLE);
    ctx.device = vk_create_device(ctx.instance, ctx.physical_device);

    return ctx;
}
