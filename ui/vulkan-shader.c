/*
 * QEMU Vulkan shader helper functions
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
#include "ui/vulkan-shader.h"
#include "ui/vulkan-helpers.h"

#include "ui/shader/texture-blit-vert-spv.h"
#include "ui/shader/texture-blit-flip-vert-spv.h"
#include "ui/shader/texture-blit-frag-spv.h"

#include <glib.h>

#define VK_CHECK(res) g_assert(res == VK_SUCCESS)

/* ---------------------------------------------------------------------- */
struct QemuVkVertex
{
    float x;
    float y;
};

// 4 vec2
static const struct QemuVkVertex in_position[] = {
    {
        .x = -1.0,
        .y = -1.0,
    },
    {
        .x = 1.0,
        .y = -1.0,
    },
    {
        .x = -1.0,
        .y = 1.0,
    },
    {
        .x = 1.0,
        .y = 1.0,
    },
};

void qemu_vk_run_texture_blit(VkCommandBuffer cmdbuf, QEMUVulkanShader *vks, bool flip)
{
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, flip ? vks->texture_blit_flip_pipeline : vks->texture_blit_pipeline);
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmdbuf, 0, 1, &vks->texture_blit_vertex_buffer.handle, offsets);
    vkCmdDraw(cmdbuf, 4, 1, 0, 0);
}

/* ---------------------------------------------------------------------- */
static VkShaderModule qemu_vk_create_compile_shader(VkDevice device, const char *src, size_t code_size)
{
    VkShaderModuleCreateInfo shader_module_create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size,
        .pCode = (const uint32_t *)src,
    };

    VkShaderModule shader;
    VK_CHECK(vkCreateShaderModule(device, &shader_module_create_info, NULL, &shader));
    return shader;
}

// layout (binding = 0) uniform sampler2D image;
static VkDescriptorSetLayout qemu_vk_create_desc_set_layout(VkDevice device)
{
    VkDescriptorSetLayoutBinding layout_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &layout_binding,
    };

    VkDescriptorSetLayout layout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, NULL, &layout));
    return layout;
}

static VkPipelineLayout qemu_vk_create_pipeline_layout(VkDevice device, VkDescriptorSetLayout set_layout)
{
    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
    };

    VkPipelineLayout pipeline_layout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));

    // TODO destroy set_layout?

    return pipeline_layout;
}

static VkPipeline qemu_vk_create_graphics_pipeline(VkDevice device, VkShaderModule vert, VkShaderModule frag, VkPipelineLayout pipeline_layout, VkRenderPass render_pass)
{
    VkPipelineShaderStageCreateInfo shader_stage_info[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };

    VkVertexInputBindingDescription vertex_input_binding_description = {
        .binding = 0,
        .stride = sizeof(struct QemuVkVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription vertex_input_attribute_description = {
        .binding = 0,
        .location = 0,
        .format = VK_FORMAT_R32G32_SFLOAT,
    };

    VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertex_input_binding_description,
        .vertexAttributeDescriptionCount = 1,
        .pVertexAttributeDescriptions = &vertex_input_attribute_description,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };

    // TODO use parameters instead of magic numbers
    VkExtent2D extent = {
        .width = 2880,
        .height = 1600,
    };

    VkViewport viewport = {
        .width = extent.width,
        .height = extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    VkRect2D scissor = {
        .extent = extent,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .lineWidth = 1.0,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 1,
        .pDynamicStates = dynamic_states,
    };

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_SIZE(shader_stage_info),
        .pStages = shader_stage_info,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = pipeline_layout,
        .renderPass = render_pass,
    };

    VkPipeline graphics_pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &graphics_pipeline));
    return graphics_pipeline;
}

static VkPipeline qemu_vk_create_pipeline_from_sources(VkDevice device,
                                                       VkPipelineLayout pipeline_layout,
                                                       VkRenderPass render_pass,
                                                       const char *vert_src, size_t vert_size,
                                                       const char *frag_src, size_t frag_size)
{
    VkShaderModule vert_shader, frag_shader;
    VkPipeline graphics_pipeline;

    vert_shader = qemu_vk_create_compile_shader(device, vert_src, vert_size);
    frag_shader = qemu_vk_create_compile_shader(device, frag_src, frag_size);
    if (vert_shader == VK_NULL_HANDLE || frag_shader == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    graphics_pipeline = qemu_vk_create_graphics_pipeline(device, vert_shader, frag_shader, pipeline_layout, render_pass);

    vkDestroyShaderModule(device, vert_shader, NULL);
    vkDestroyShaderModule(device, frag_shader, NULL);

    return graphics_pipeline;
}

static VkRenderPass qemu_vk_init_texture_blit_render_pass(VkDevice device, VkFormat color_attachment_format)
{
    VkAttachmentDescription color_attachment = {
        .format = color_attachment_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    VkAttachmentReference color_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref,
    };

    // Describes the transition for the color attachment
    VkSubpassDependency subpass_dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        // Wait until we are in the color attachment output stage
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        // Transition before writing to the color attachment
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo render_pass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &subpass_dependency,
    };

    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(device, &render_pass_create_info, NULL, &render_pass));
    return render_pass;
}

/* ---------------------------------------------------------------------- */

QEMUVulkanShader *qemu_vk_init_shader(QEMUVkDevice device, VkFormat color_attachment_format)
{
    QEMUVulkanShader *vks = g_new0(QEMUVulkanShader, 1);

    vks->desc_set_layout = qemu_vk_create_desc_set_layout(device.handle);
    vks->texture_blit_pipeline_layout = qemu_vk_create_pipeline_layout(device.handle, vks->desc_set_layout);
    vks->texture_blit_render_pass = qemu_vk_init_texture_blit_render_pass(device.handle, color_attachment_format);

    vks->texture_blit_pipeline =
        qemu_vk_create_pipeline_from_sources(device.handle,
                                             vks->texture_blit_pipeline_layout,
                                             vks->texture_blit_render_pass,
                                             texture_blit_vert_src, ARRAY_SIZE(texture_blit_vert_src),
                                             texture_blit_frag_src, ARRAY_SIZE(texture_blit_frag_src));
    vks->texture_blit_flip_pipeline =
        qemu_vk_create_pipeline_from_sources(device.handle,
                                             vks->texture_blit_pipeline_layout,
                                             vks->texture_blit_render_pass,
                                             texture_blit_flip_vert_src, ARRAY_SIZE(texture_blit_flip_vert_src),
                                             texture_blit_frag_src, ARRAY_SIZE(texture_blit_frag_src));
    g_assert(vks->texture_blit_pipeline != VK_NULL_HANDLE &&
             vks->texture_blit_flip_pipeline != VK_NULL_HANDLE);

    vks->texture_blit_vertex_buffer = vk_create_buffer(device, sizeof(in_position), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* data;
    vkMapMemory(device.handle, vks->texture_blit_vertex_buffer.memory, 0, sizeof(in_position), 0, &data);
    memcpy(data, in_position, sizeof(in_position));
    vkUnmapMemory(device.handle, vks->texture_blit_vertex_buffer.memory);

    return vks;
}

void qemu_vk_fini_shader(VkDevice device, QEMUVulkanShader *vks)
{
    if (!vks)
    {
        return;
    }

    vkDestroyRenderPass(device, vks->texture_blit_render_pass, NULL);
    vkDestroyPipeline(device, vks->texture_blit_flip_pipeline, NULL);
    vkDestroyPipeline(device, vks->texture_blit_pipeline, NULL);
    vk_destroy_buffer(device, vks->texture_blit_vertex_buffer);
    vkDestroyPipelineLayout(device, vks->texture_blit_pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, vks->desc_set_layout, NULL);

    g_free(vks);
}
