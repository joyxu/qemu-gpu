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

VkInstance instance;

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
    g_print("[Vulkan validation]: %s - %s\n", layer_prefix, message);
    return VK_FALSE;
}

VkDevice vk_init(void)
{
    VkDevice device = VK_NULL_HANDLE;

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
    VkPhysicalDevice* physical_devices = g_new(VkPhysicalDevice, device_count);

    VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, physical_devices));

    // TODO: select physical device based on user input
    VkPhysicalDevice physical_device = physical_devices[1];
    g_free(physical_devices);

    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical_device, &device_properties);
    g_print("Bringing up Vulkan on %s\n", device_properties.deviceName);

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    };

    VK_CHECK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));

    return device;
}
