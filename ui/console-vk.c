/*
 * QEMU graphical console -- Vulkan helper bits
 *
 * Copyright (c) 2021 Collabora Ltd.
 *
 * Authors:
 *    Antonio Caggiano <antonio.caggiano@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "ui/console.h"

#define VK_CHECK(res) g_assert(res == VK_SUCCESS)

/* ---------------------------------------------------------------------- */

bool console_vk_check_format(pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
    case PIXMAN_r5g6b5:
        return true;
    default:
        return false;
    }
}

uint32_t get_memory_type_index(QEMUVkPhysicalDevice physical_device,
                               uint32_t memory_type_filter,
                               VkMemoryPropertyFlags memory_property_flags)
{
    for (uint32_t i = 0; i < physical_device.memory_properties.memoryTypeCount; ++i) {
        if (memory_type_filter & (1 << i)
            && ((physical_device.memory_properties.memoryTypes[i].propertyFlags & memory_property_flags) == memory_property_flags)
        ) {
            return i;
        }
    }

    g_assert(false);
    return 0;
}

typedef struct QEMUVkBuffer {
    VkBuffer handle;
    VkDeviceMemory memory;
} QEMUVkBuffer;

static QEMUVkBuffer vk_create_buffer(QEMUVkDevice device,
                              VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags memory_properties)
{
    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    QEMUVkBuffer buffer;
    VK_CHECK(vkCreateBuffer(device.handle, &buffer_create_info, NULL, &buffer.handle));

    VkMemoryRequirements memory_requirements;
    vkGetBufferMemoryRequirements(device.handle, buffer.handle, &memory_requirements);

    VkMemoryAllocateInfo memory_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = get_memory_type_index(device.physical_device, memory_requirements.memoryTypeBits, memory_properties)
    };

    VK_CHECK(vkAllocateMemory(device.handle, &memory_alloc_info, NULL, &buffer.memory));
    VK_CHECK(vkBindBufferMemory(device.handle, buffer.handle, buffer.memory, 0));

    return buffer;
}

static vk_destroy_buffer(VkDevice device, QEMUVkBuffer buffer) {
    vkDestroyBuffer(device, buffer.handle, NULL);
    vkFreeMemory(device, buffer.memory, NULL);
}

static VkCommandBuffer vk_command_buffer_submit(VkCommandBuffer command_buffer, VkQueue queue) {
    vkEndCommandBuffer(command_buffer);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
    };

    VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));

    // TODO wait for fence
    vkQueueWaitIdle(queue);
}

static vk_image_transition(VkCommandBuffer cmd_buf, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout)
{
    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;

    VkAccessFlags src_access_mask;
    VkAccessFlags dst_access_mask;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED
        && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            // Do not wait for anything before starting the transition
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            src_access_mask = 0;
            // Complete the transition before transfer stage
            dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst_access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
               && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        // Wait transfer has finished
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        src_access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;

        // Complete layout transition before shader read stage
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dst_access_mask = VK_ACCESS_SHADER_READ_BIT;
    } else {
        g_assert(false); // TODO FIXME transition not supported for the moment
    }


    VkImageMemoryBarrier image_memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseArrayLayer = 0,
            .layerCount = 1,
            .baseMipLevel = 0,
            .levelCount = 1,
        },
        .srcAccessMask = src_access_mask,
        .dstAccessMask = dst_access_mask,
    };

    vkCmdPipelineBarrier(cmd_buf, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier);
}


static vk_copy_buffer_to_image(VkCommandBuffer cmd_buf, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
    VkBufferImageCopy buffer_image_copy = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {
            .x = 0,
            .y = 0,
            .z = 0,
        },
        .imageExtent = {
            .width = width,
            .height = height,
            .depth = 1
        },
    };

    vkCmdCopyBufferToImage(cmd_buf, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &buffer_image_copy);
}

// TODO: Is this the swapchain image?
/// I believe this is a texture created to upload data from the display surface
/// This texture will probably be rendered later with the blit pipelines.
void surface_vk_create_texture(QEMUVkDevice device,
                               DisplaySurface *surface)
{
    // TODO: what about the format here?
    switch (surface->format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        surface->vk_format = VK_FORMAT_B8G8R8A8_UNORM;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        surface->vk_format = VK_FORMAT_R8G8B8A8_UNORM;
        break;
    case PIXMAN_r5g6b5:
        surface->vk_format = VK_FORMAT_R5G6B5_UNORM_PACK16;
        break;
    default:
        g_assert_not_reached();
    }

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = surface->vk_format,
        .extent = {
            .width = surface_width(surface),
            .height = surface_height(surface),
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, // TODO: figure out usage
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VK_CHECK(vkCreateImage(device.handle, &image_create_info, NULL, &surface->vk_image));

    // Memory for the image
    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device.handle, surface->vk_image, &memory_requirements);

    VkMemoryAllocateInfo memory_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = get_memory_type_index(device.physical_device, memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    VK_CHECK(vkAllocateMemory(device.handle, &memory_alloc_info, NULL, &surface->vk_memory));
    VK_CHECK(vkBindImageMemory(device.handle, surface->vk_image, surface->vk_memory, 0));

    VkImageViewCreateInfo view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = surface->vk_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = surface->vk_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }
    };

    VK_CHECK(vkCreateImageView(device.handle, &view_create_info, NULL, &surface->vk_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
    };

    VK_CHECK(vkCreateSampler(device.handle, &sampler_create_info, NULL, &surface->vk_sampler));

    uint32_t width = surface_width(surface);
    uint32_t height = surface_height(surface);
    uint32_t surface_size = surface_bytes_per_pixel(surface) * width * height;

    // TODO upload data from surface to a staging buffer then to the image
    QEMUVkBuffer staging_buffer = vk_create_buffer(device, surface_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* data;
    VK_CHECK(vkMapMemory(device.handle, staging_buffer.memory, 0, surface_size, 0, &data));
    memcpy(data, surface_data(surface), surface_size);
    vkUnmapMemory(device.handle, staging_buffer.memory);

    // TODO need a command buffer for a graphics queue
    vk_command_buffer_begin(device.general_command_buffer, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    vk_image_transition(device.general_command_buffer, surface->vk_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_copy_buffer_to_image(device.general_command_buffer, staging_buffer.handle, surface->vk_image, width, height);
    vk_image_transition(device.general_command_buffer, surface->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vk_command_buffer_submit(device.general_command_buffer, device.graphics_queue);

    vk_destroy_buffer(device.handle, staging_buffer);
}

void surface_vk_update_texture(VkCommandBuffer cmdbuf,
                               QEMUVulkanShader *vks,
                               DisplaySurface *surface,
                               int x, int y, int w, int h)
{
    uint8_t *data = (void *)surface_data(surface);

    // TODO need a command buffer for a graphics queue

    if (surface->texture) {
        //glBindTexture(GL_TEXTURE_2D, surface->texture);
        //glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
        //              surface_stride(surface)
        //              / surface_bytes_per_pixel(surface));
        //glTexSubImage2D(GL_TEXTURE_2D, 0,
        //                x, y, w, h,
        //                surface->glformat, surface->gltype,
        //                data + surface_stride(surface) * y
        //                + surface_bytes_per_pixel(surface) * x);
    }
}

static VkSemaphore vk_create_semaphore(VkDevice device) {
    VkSemaphoreCreateInfo semaphore_create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphore;
    VK_CHECK(vkCreateSemaphore(device, &semaphore_create_info, NULL, &semaphore));
    return semaphore;
}

static uint32_t frame_index = 0;

void surface_vk_render_texture(QEMUVkDevice device,
                               QEMUVkSwapchain swapchain,
                               vulkan_fb *fb,
                               VkCommandBuffer cmd_buf,
                               QEMUVulkanShader *vks,
                               DisplaySurface *surface)
{
    assert(vks);

    VkClearValue clear = {
        .color = {
            .float32 = {0.1f, 0.1f, 0.1f, 0.0f}
        },
    };

    g_assert(swapchain.image_count >= 0);
    g_assert(fb->framebuffers != NULL);

    VkRenderPassBeginInfo render_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vks->texture_blit_render_pass,
        .framebuffer = fb->framebuffers[frame_index],
        .renderArea = {
            .extent = {
                .width = surface_width(surface),
                .height = surface_height(surface),
            }
        },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    
    frame_index = (frame_index + 1) % swapchain.image_count;

    vkCmdBeginRenderPass(cmd_buf, &render_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    qemu_vk_run_texture_blit(cmd_buf, vks, false);

    vkCmdEndRenderPass(cmd_buf);

    VK_CHECK(vkEndCommandBuffer(cmd_buf));

    // TODO: queue command buffer?
    // TODO: present?

    VkSemaphore image_available_semaphore = vk_create_semaphore(device.handle);
    VkSemaphore render_finished_semaphore = vk_create_semaphore(device.handle);

    uint32_t image_index;
    // Image available sempahore will be signaled once the image is correctly acquired
    vkAcquireNextImageKHR(device.handle, swapchain.handle, UINT64_MAX, image_available_semaphore, VK_NULL_HANDLE, &image_index);

    // Wait the swapchain image has been acquired before writing color onto it
    VkSemaphore wait_semaphores[] = { image_available_semaphore };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    // Signal this semaphore once the commands have finished execution
    VkSemaphore signal_semaphores[] = { render_finished_semaphore };

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = wait_semaphores,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd_buf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signal_semaphores,
    };

    VK_CHECK(vkQueueSubmit(device.graphics_queue, 1, &submit_info, VK_NULL_HANDLE));

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        // Wait for commands to finish before presenting the result
        .pWaitSemaphores = signal_semaphores,
        .swapchainCount = 1,
        .pSwapchains = swapchain.handle,
        .pImageIndices = &image_index,
    };

    VK_CHECK(vkQueuePresentKHR(device.present_queue, &present_info));

    vkQueueWaitIdle(device.graphics_queue);

    vkDestroySemaphore(device.handle, image_available_semaphore, NULL);
    vkDestroySemaphore(device.handle, render_finished_semaphore, NULL);
}

void surface_vk_destroy_texture(VkDevice device,
                                DisplaySurface *surface)
{
    if (!surface || surface->vk_image == VK_NULL_HANDLE) {
        return;
    }
    
    vkDestroySampler(device, surface->vk_sampler, NULL);
    surface->vk_sampler = VK_NULL_HANDLE;
    vkDestroyImageView(device, surface->vk_view, NULL);
    surface->vk_view = VK_NULL_HANDLE;
    vkDestroyImage(device, surface->vk_image, NULL);
    surface->vk_image = VK_NULL_HANDLE;
    // TODO destroy framebuffer?
}

void surface_vk_setup_viewport(VkCommandBuffer cmdbuf,
                               DisplaySurface *surface,
                               int ww, int wh)
{
    int gw, gh, stripe;
    float sw, sh;

    gw = surface_width(surface);
    gh = surface_height(surface);

    sw = (float)ww/gw;
    sh = (float)wh/gh;

    VkViewport viewport = {};

    if (sw < sh) {
        stripe = wh - wh*sw/sh;
        viewport.x = 0;
        viewport.y = stripe /2;
        viewport.width = ww;
        viewport.height = wh - stripe;
    } else {
        stripe = ww - ww*sh/sw;
        viewport.x = stripe / 2;
        viewport.y = 0;
        viewport.width = ww - stripe;
        viewport.height = wh;
    }

    vkCmdSetViewport(cmdbuf, 0, 1, &viewport);
}
