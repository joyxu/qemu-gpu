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

void vulkan_init(void)
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

    const char* validation_layers[] = { "VK_LAYER_KHRONOS_validation" };
    uint32_t layer_count = 1;

    // Check layers availability
    uint32_t instance_layer_count;
    VK_CHECK(vkEnumerateInstanceLayerProperties(&instance_layer_count, NULL));

    VkLayerProperties* instance_layers = g_new(VkLayerProperties, instance_layer_count);
    VK_CHECK(vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layers));

    bool layers_available = true;

    for (uint32_t i = 0; i < layer_count; ++i) {
        bool current_layer_available = false;
        for (uint32_t j = 0; j < instance_layer_count; ++j) {
            g_print("%s\n", instance_layers[j].layerName);
            if (strcmp(instance_layers[j].layerName, validation_layers[i]) == 0) {
                current_layer_available = true;
                break;
            }
        }
        if (!current_layer_available) {
            layers_available = false;
            break;
        }
    }

    // TODO: remove and enable validation layers only on debug mode
    g_assert(layers_available);

    // Setup debug report
    const char* validation_ext = VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
    if (layers_available) {
        instance_create_info.ppEnabledLayerNames = validation_layers;
        instance_create_info.enabledLayerCount= layer_count;
        instance_create_info.ppEnabledExtensionNames = &validation_ext;
        instance_create_info.enabledExtensionCount = 1;
    }


    VK_CHECK(vkCreateInstance(&instance_create_info, NULL, &instance));

    g_free(instance_layers);
}
