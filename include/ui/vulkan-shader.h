#ifndef QEMU_VK_SHADER_H
#define QEMU_VK_SHADER_H

#include <vulkan/vulkan_core.h>

typedef struct QemuVkShader QemuVkShader;

void qemu_vk_run_texture_blit(QemuVkShader *gls, bool flip);

QemuVkShader *qemu_vk_init_shader(VkDevice device);
void qemu_vk_fini_shader(VkDevice device, QemuVkShader *gls);

#endif /* QEMU_VK_SHADER_H */
