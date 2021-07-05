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


static void vk_command_buffer_submit(VkCommandBuffer* command_buffer, VkQueue queue) {
    vkEndCommandBuffer(*command_buffer);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = command_buffer,
    };

    VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));

    // TODO wait for fence
    vkQueueWaitIdle(queue);
}

static void vk_image_transition(VkCommandBuffer cmd_buf, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout)
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


static void vk_copy_buffer_to_image(VkCommandBuffer cmd_buf, VkBuffer buffer, uint32_t buffer_row_length, VkImage image, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    VkBufferImageCopy buffer_image_copy = {
        .bufferOffset = 0,
        .bufferRowLength = buffer_row_length,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {
            .x = x,
            .y = y,
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
        },
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
    surface_vk_update_texture(device, surface, 0, 0, width, height);
}

void surface_vk_update_texture(QEMUVkDevice device,
                               DisplaySurface *surface,
                               int x, int y, int w, int h)
{
    uint8_t *pixel_data = (void *)surface_data(surface)
        + surface_stride(surface) * y
        + surface_bytes_per_pixel(surface) * x;

    uint32_t width = w;
    uint32_t height = h;
    uint32_t surface_size = surface_stride(surface) * height;
    QEMUVkBuffer staging_buffer = vk_create_buffer(device, surface_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* data;
    VK_CHECK(vkMapMemory(device.handle, staging_buffer.memory, 0, surface_size, 0, &data));
    memcpy(data, pixel_data, surface_size);
    vkUnmapMemory(device.handle, staging_buffer.memory);

    vk_command_buffer_begin(device.general_command_buffer, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    vk_image_transition(device.general_command_buffer, surface->vk_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // in texels
    uint32_t buffer_row_length = surface_stride(surface) / surface_bytes_per_pixel(surface);
    vk_copy_buffer_to_image(device.general_command_buffer, staging_buffer.handle, buffer_row_length, surface->vk_image, x, y, width, height);
    vk_image_transition(device.general_command_buffer, surface->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vk_command_buffer_submit(&device.general_command_buffer, device.graphics_queue);

    vk_destroy_buffer(device.handle, staging_buffer);
}

static VkSemaphore vk_create_semaphore(VkDevice device) {
    VkSemaphoreCreateInfo semaphore_create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphore;
    VK_CHECK(vkCreateSemaphore(device, &semaphore_create_info, NULL, &semaphore));
    return semaphore;
}

void surface_vk_render_texture(QEMUVkDevice device,
                               QEMUVkSwapchain* swapchain,
                               QEMUVkFrames *frames,
                               QEMUVulkanShader *vks,
                               DisplaySurface *surface)
{
    assert(vks);
    
    VkDescriptorImageInfo desc_image_info = {
        .imageView = surface->vk_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .sampler = surface->vk_sampler,
    };

    VkDescriptorSet desc_set = frames->desc_sets[frames->frame_index];

    VkWriteDescriptorSet write_desc_set = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = desc_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &desc_image_info,
    };

    vkUpdateDescriptorSets(device.handle, 1, &write_desc_set, 0, NULL);

    VkClearValue clear = {
        .color = {
            .float32 = {0.1f, 0.1f, 0.1f, 1.0f}
        },
    };

    VkRenderPassBeginInfo render_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vks->texture_blit_render_pass,
        .framebuffer = frames->framebuffers[frames->frame_index],
        .renderArea = {
            .extent = {
                .width = swapchain->extent.width,
                .height = swapchain->extent.height,
            }
        },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };

    VkCommandBuffer cmd_buf = frames->cmd_bufs[frames->frame_index];
    
    vkCmdBeginRenderPass(cmd_buf, &render_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vks->texture_blit_pipeline_layout, 0, 1, &desc_set, 0, NULL);

    qemu_vk_run_texture_blit(cmd_buf, vks, false);

    vkCmdEndRenderPass(cmd_buf);

    VK_CHECK(vkEndCommandBuffer(cmd_buf));

    VkSemaphore image_available_semaphore = vk_create_semaphore(device.handle);
    VkSemaphore render_finished_semaphore = vk_create_semaphore(device.handle);

    uint32_t image_index;
    // Image available sempahore will be signaled once the image is correctly acquired
    VkResult acquire_res = vkAcquireNextImageKHR(device.handle, swapchain->handle, UINT64_MAX, image_available_semaphore, VK_NULL_HANDLE, &image_index);

    if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR) {
        // TODO extract to function
        vk_swapchain_recreate(device, swapchain, 0, 0);

        for (uint32_t i = 0; i < swapchain->image_count; ++i) {
            vkDestroyFramebuffer(device.handle, frames->framebuffers[i], NULL);
            frames->framebuffers[i] = vk_create_framebuffer(device.handle, vks->texture_blit_render_pass, swapchain->views[i], swapchain->extent.width, swapchain->extent.height);
        }

        frames->frame_index = 0;

        // TODO Skip this frame? Try acquiring the swapchain again?
        return;
    }

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
    
    VK_CHECK(vkQueueWaitIdle(device.graphics_queue));

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        // Wait for commands to finish before presenting the result
        .pWaitSemaphores = signal_semaphores,
        .swapchainCount = 1,
        .pSwapchains = &swapchain->handle,
        .pImageIndices = &image_index,
    };

    VkResult present_res = vkQueuePresentKHR(device.present_queue, &present_info);
    if (present_res == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_swapchain_recreate(device, swapchain, 0, 0);

        for (uint32_t i = 0; i < swapchain->image_count; ++i) {
            vkDestroyFramebuffer(device.handle, frames->framebuffers[i], NULL);
            frames->framebuffers[i] = vk_create_framebuffer(device.handle, vks->texture_blit_render_pass, swapchain->views[i], swapchain->extent.width, swapchain->extent.height);
        }

        frames->frame_index = 0;
        return;
    }

    VK_CHECK(vkQueueWaitIdle(device.present_queue));

    vkDestroySemaphore(device.handle, image_available_semaphore, NULL);
    vkDestroySemaphore(device.handle, render_finished_semaphore, NULL);

    // Advance to the next frame
    frames->frame_index = (frames->frame_index + 1) % swapchain->image_count;
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
}

void surface_vk_setup_viewport(VkCommandBuffer cmdbuf,
                               DisplaySurface *surface,
                               int ww, int wh)
{
    ww *= 3;
    wh *= 3;

    int gw, gh, stripe;
    float sw, sh;

    gw = surface_width(surface);
    gh = surface_height(surface);

    sw = (float)ww / gw;
    sh = (float)wh / gh;

    VkViewport viewport = {
        .minDepth = 0.0,
        .maxDepth = 1.0,
    };

    if (sw < sh) {
        stripe = wh - wh * sw / sh;
        viewport.x = 0;
        viewport.y = stripe / 2;
        viewport.width = ww;
        viewport.height = wh - stripe;
    } else {
        stripe = ww - ww * sh / sw;
        viewport.x = stripe / 2;
        viewport.y = 0;
        viewport.width = ww - stripe;
        viewport.height = wh;
    }

    vkCmdSetViewport(cmdbuf, 0, 1, &viewport);

    VkRect2D scissor = {
        .extent = {
            .width = viewport.width,
            .height = viewport.height,
        },
        .offset = {
            .x = viewport.x,
            .y = viewport.y
        }
    };

    vkCmdSetScissor(cmdbuf, 0, 1, &scissor);
}
