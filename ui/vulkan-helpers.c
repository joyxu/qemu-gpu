/*
 * Copyright (C) 2021 Collabora Ltd.
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

#define VK_CHECK(res) g_assert(res == VK_SUCCESS)

// TODO where do we destroy these?
VkInstance instance;
VkDevice device;

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

        texture->delete_image = false;
    }

    if (texture->view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, texture->view, NULL);
        texture->view = VK_NULL_HANDLE;
    }

    texture->width = 0;
    texture->height = 0;
}

void vk_fb_destroy(VkDevice device, vulkan_fb *fb)
{
    if (fb->framebuffer == VK_NULL_HANDLE)
    {
        return;
    }

    vk_texture_destroy(device, &fb->texture);
    vkDestroyFramebuffer(device, fb->framebuffer, NULL);
    fb->framebuffer = VK_NULL_HANDLE;
}

void vk_fb_setup_for_tex(VkDevice device, vulkan_fb *fb, vulkan_texture texture)
{
    // Destroy previous texture and then set the new one
    vk_texture_destroy(device, &fb->texture);
    fb->texture = texture;

    if (fb->framebuffer == VK_NULL_HANDLE)
    {
        VkFramebufferCreateInfo framebuffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = VK_NULL_HANDLE,
            .attachmentCount = 1,
            .pAttachments = &fb->texture.view,
            .width = fb->texture.width,
            .height = fb->texture.height,
            .layers = 1,
        };

        VK_CHECK(vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &fb->framebuffer));
    }
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

VkDevice vk_init(void)
{
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Qemu headless",
        .pEngineName = "Qemu",
        .apiVersion = VK_API_VERSION_1_0,
    };

    VkInstanceCreateInfo instance_create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    const char *validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
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

    const char *validation_ext = VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
    if (layers_available)
    {
        instance_create_info.ppEnabledLayerNames = validation_layers;
        instance_create_info.enabledLayerCount = layer_count;
        instance_create_info.ppEnabledExtensionNames = &validation_ext;
        instance_create_info.enabledExtensionCount = 1;
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

    // Create device
    uint32_t device_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, NULL));
    VkPhysicalDevice *physical_devices = g_new(VkPhysicalDevice, device_count);

    VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, physical_devices));

    // TODO: select physical device based on user input
    VkPhysicalDevice physical_device = physical_devices[1];
    g_free(physical_devices);

    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical_device, &device_properties);
    g_print("Bringing up Vulkan on %s\n", device_properties.deviceName);

    // Request a single graphics queue
    uint32_t queue_family_index = 0;
    const float default_queue_priority = 0.0f;
    VkDeviceQueueCreateInfo queue_create_info = {};
    uint32_t queue_family_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);
    VkQueueFamilyProperties *queue_family_properties = g_new(VkQueueFamilyProperties, queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_family_properties);
    for (uint32_t i = 0; i < queue_family_count; i++)
    {
        if (queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            queue_family_index = i;
            queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_create_info.queueFamilyIndex = i;
            queue_create_info.queueCount = 1;
            queue_create_info.pQueuePriorities = &default_queue_priority;
            break;
        }
    }

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
    };

    VK_CHECK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));

    return device;
}
