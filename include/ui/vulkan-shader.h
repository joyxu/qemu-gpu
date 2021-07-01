#ifndef QEMU_VK_SHADER_H
#define QEMU_VK_SHADER_H

#include <vulkan/vulkan_core.h>

typedef struct QEMUVulkanShader
{
    VkRenderPass texture_blit_render_pass;
    VkPipeline texture_blit_pipeline;
    VkPipeline texture_blit_flip_pipeline;
    VkBuffer texture_blit_vertex_buffer;
} QEMUVulkanShader;

void qemu_vk_run_texture_blit(VkCommandBuffer cmdbuf, QEMUVulkanShader *gls, bool flip);

QEMUVulkanShader *qemu_vk_init_shader(VkDevice device, VkFormat color_attachment_format);
void qemu_vk_fini_shader(VkDevice device, QEMUVulkanShader *gls);

#endif /* QEMU_VK_SHADER_H */
