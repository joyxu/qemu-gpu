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

// TODO: Is this the swapchain image?
/// I believe this is a texture created to upload data from the display surface
/// This texture will probably be rendered later with the blit pipelines.
void surface_vk_create_texture(QEMUVkDevice device,
                               QEMUVkSwapchain swapchain,
                               DisplaySurface *surface)
{
    // TODO: what about the format here?
    switch (surface->format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        surface->vkformat = VK_FORMAT_B8G8R8A8_UNORM;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        surface->vkformat = VK_FORMAT_R8G8B8A8_UNORM;
        break;
    case PIXMAN_r5g6b5:
        surface->vkformat = VK_FORMAT_R5G6B5_UNORM_PACK16;
        break;
    default:
        g_assert_not_reached();
    }

    // TODO: support multiple swapchain images?

    VkImageViewCreateInfo view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = swapchain.images[0],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = swapchain.format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }
    };

    VK_CHECK(vkCreateImageView(device.handle, &view_create_info, NULL, &surface->vkview));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
    };

    VK_CHECK(vkCreateSampler(device.handle, &sampler_create_info, NULL, &surface->vksampler));

    // TODO upload data from surface to the image?
    // TODO need a command buffer for a graphics queue
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

void surface_vk_render_texture(VkCommandBuffer cmdbuf,
                               QEMUVulkanShader *vks,
                               DisplaySurface *surface)
{
    assert(vks);

    VkCommandBufferBeginInfo cmdbuf_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    VK_CHECK(vkBeginCommandBuffer(cmdbuf, &cmdbuf_begin_info));

    VkClearValue clear = {
        .color = {
            .float32 = {0.1f, 0.1f, 0.1f, 0.0f}
        },
    };

    VkRenderPassBeginInfo render_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = VK_NULL_HANDLE, // TODO Where do we put the renderpass?
        .framebuffer = surface->vkframebuffer,
        .renderArea = {
            .extent = {
                .width = surface_width(surface),
                .height = surface_height(surface),
            }
        },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    

    vkCmdBeginRenderPass(cmdbuf, &render_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    qemu_vk_run_texture_blit(cmdbuf, vks, false);

    vkCmdEndRenderPass(cmdbuf);

    VK_CHECK(vkEndCommandBuffer(cmdbuf));

    // TODO: queue command buffer?
    // TODO: present?
}

void surface_vk_destroy_texture(VkDevice device,
                                DisplaySurface *surface)
{
    if (!surface || surface->vkimage == VK_NULL_HANDLE) {
        return;
    }
    
    vkDestroySampler(device, surface->vksampler, NULL);
    surface->vksampler = VK_NULL_HANDLE;
    vkDestroyImageView(device, surface->vkview, NULL);
    surface->vkview = VK_NULL_HANDLE;
    vkDestroyImage(device, surface->vkimage, NULL);
    surface->vkimage = VK_NULL_HANDLE;
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
