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

#include "ui/shader/texture-blit-vert-spv.h"
#include "ui/shader/texture-blit-flip-vert-spv.h"
#include "ui/shader/texture-blit-frag-spv.h"

#include <vulkan/vulkan.h>

#include <glib.h>

#define VK_CHECK(res) g_assert(res == VK_SUCCESS)

struct QemuVkShader
{
    VkPipeline texture_blit_prog;
    VkPipeline texture_blit_flip_prog;
    VkBuffer texture_blit_vao;
};

#if 0
/* ---------------------------------------------------------------------- */

static GLuint qemu_vk_init_texture_blit(GLint texture_blit_prog)
{
    static const GLfloat in_position[] = {
        -1, -1,
        1,  -1,
        -1,  1,
        1,   1,
    };
    GLint l_position;
    GLuint vao, buffer;

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    /* this is the VBO that holds the vertex data */
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(in_position), in_position,
                 GL_STATIC_DRAW);

    l_position = glGetAttribLocation(texture_blit_prog, "in_position");
    glVertexAttribPointer(l_position, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(l_position);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    return vao;
}

void qemu_vk_run_texture_blit(QemuVkShader *gls, bool flip)
{
    glUseProgram(flip
                 ? gls->texture_blit_flip_prog
                 : gls->texture_blit_prog);
    glBindVertexArray(gls->texture_blit_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ---------------------------------------------------------------------- */
#endif
static VkShaderModule qemu_vk_create_compile_shader(VkDevice device, const char *src, size_t code_size)
{
    VkShaderModule shader;

    VkShaderModuleCreateInfo shader_module_create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size,
        .pCode = (const uint32_t *)src,
    };

    VK_CHECK(vkCreateShaderModule(device, &shader_module_create_info, NULL, &shader));

    /*
    GLint status, length;
    char *errmsg;

    shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, 0);
    glCompileShader(shader);

    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        errmsg = g_malloc(length);
        glGetShaderInfoLog(shader, length, &length, errmsg);
        fprintf(stderr, "%s: compile %s error\n%s\n", __func__,
                (type == GL_VERTEX_SHADER) ? "vertex" : "fragment",
                errmsg);
        g_free(errmsg);
        return 0;
    }
    */
    return shader;
}

#if 0

static GLuint qemu_vk_create_link_program(GLuint vert, GLuint frag)
{
    GLuint program;
    GLint status, length;
    char *errmsg;

    program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        errmsg = g_malloc(length);
        glGetProgramInfoLog(program, length, &length, errmsg);
        fprintf(stderr, "%s: link program: %s\n", __func__, errmsg);
        g_free(errmsg);
        return 0;
    }
    return program;
}
#endif

static VkPipeline qemu_vk_create_compile_link_program(VkDevice device,
                                                      const char *vert_src, size_t vert_size,
                                                      const char *frag_src, size_t frag_size)
{
    VkShaderModule vert_shader, frag_shader;
    VkPipeline program;

    vert_shader = qemu_vk_create_compile_shader(device, vert_src, vert_size);
    frag_shader = qemu_vk_create_compile_shader(device, frag_src, frag_size);
    if (vert_shader == VK_NULL_HANDLE || frag_shader == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    // TODO
    //program = qemu_vk_create_link_program(vert_shader, frag_shader);
    vkDestroyShaderModule(device, vert_shader, NULL);
    vkDestroyShaderModule(device, frag_shader, NULL);

    return program;
}

/* ---------------------------------------------------------------------- */

QemuVkShader *qemu_vk_init_shader(VkDevice device)
{
    QemuVkShader *vks = g_new0(QemuVkShader, 1);

    vks->texture_blit_prog = qemu_vk_create_compile_link_program(device,
                                                                 texture_blit_vert_src, ARRAY_SIZE(texture_blit_vert_src),
                                                                 texture_blit_frag_src, ARRAY_SIZE(texture_blit_frag_src));
    vks->texture_blit_flip_prog = qemu_vk_create_compile_link_program(device,
                                                                      texture_blit_flip_vert_src, ARRAY_SIZE(texture_blit_flip_vert_src),
                                                                      texture_blit_frag_src, ARRAY_SIZE(texture_blit_frag_src));
    g_assert(vks->texture_blit_prog != VK_NULL_HANDLE && vks->texture_blit_flip_prog != VK_NULL_HANDLE);

    // TODO
    // vks->texture_blit_vao = qemu_vk_init_texture_blit(vks->texture_blit_prog);

    return vks;
}

void qemu_vk_fini_shader(VkDevice device, QemuVkShader *vks)
{
    if (!vks)
    {
        return;
    }

    vkDestroyPipeline(device, vks->texture_blit_flip_prog, NULL);
    vkDestroyPipeline(device, vks->texture_blit_prog, NULL);

    g_free(vks);
}
