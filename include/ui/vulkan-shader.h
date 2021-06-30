#ifndef QEMU_VK_SHADER_H
#define QEMU_VK_SHADER_H

#include <vulkan/vulkan_core.h>

typedef struct QEMUVulkanShader QEMUVulkanShader;

void qemu_vk_run_texture_blit(VkCommandBuffer cmdbuf, QEMUVulkanShader *gls, bool flip);

QEMUVulkanShader *qemu_vk_init_shader(VkDevice device);
void qemu_vk_fini_shader(QEMUVulkanShader *gls);

#endif /* QEMU_VK_SHADER_H */
