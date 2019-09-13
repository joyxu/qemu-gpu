/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "hw/virtio/virtio-gpu.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "trace.h"

void
virtio_gpu_base_reset(VirtIOGPUBase *g)
{
    int i;

    g->enable = 0;
    g->use_virgl_renderer = false;

    for (i = 0; i < g->conf.max_outputs; i++) {
        g->scanout[i].resource_id = 0;
        g->scanout[i].width = 0;
        g->scanout[i].height = 0;
        g->scanout[i].x = 0;
        g->scanout[i].y = 0;
        g->scanout[i].ds = NULL;
    }
}

void
virtio_gpu_base_fill_display_info(VirtIOGPUBase *g,
                                  struct virtio_gpu_resp_display_info *dpy_info)
{
    int i;

    for (i = 0; i < g->conf.max_outputs; i++) {
        if (g->enabled_output_bitmask & (1 << i)) {
            dpy_info->pmodes[i].enabled = 1;
            dpy_info->pmodes[i].r.width = cpu_to_le32(g->req_state[i].width);
            dpy_info->pmodes[i].r.height = cpu_to_le32(g->req_state[i].height);
        }
    }
}

static void virtio_gpu_invalidate_display(void *opaque)
{
}

static void virtio_gpu_update_display(void *opaque)
{
}

static void virtio_gpu_text_update(void *opaque, console_ch_t *chardata)
{
}

static void virtio_gpu_notify_event(VirtIOGPUBase *g, uint32_t event_type)
{
    g->virtio_config.events_read |= event_type;
    virtio_notify_config(&g->parent_obj);
}

static int virtio_gpu_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    VirtIOGPUBase *g = opaque;

    if (idx >= g->conf.max_outputs) {
        return -1;
    }

    g->req_state[idx].x = info->xoff;
    g->req_state[idx].y = info->yoff;
    g->req_state[idx].width = info->width;
    g->req_state[idx].height = info->height;

    if (info->width && info->height) {
        g->enabled_output_bitmask |= (1 << idx);
    } else {
        g->enabled_output_bitmask &= ~(1 << idx);
    }

    /* send event to guest */
    virtio_gpu_notify_event(g, VIRTIO_GPU_EVENT_DISPLAY);
    return 0;
}

static void
virtio_gpu_gl_block(void *opaque, bool block)
{
    VirtIOGPUBase *g = opaque;
    VirtIOGPUBaseClass *vgc = VIRTIO_GPU_BASE_GET_CLASS(g);

    if (block) {
        g->renderer_blocked++;
    } else {
        g->renderer_blocked--;
    }
    assert(g->renderer_blocked >= 0);

    if (g->renderer_blocked == 0) {
        vgc->gl_unblock(g);
    }
}

const GraphicHwOps virtio_gpu_ops = {
    .invalidate = virtio_gpu_invalidate_display,
    .gfx_update = virtio_gpu_update_display,
    .text_update = virtio_gpu_text_update,
    .ui_info = virtio_gpu_ui_info,
    .gl_block = virtio_gpu_gl_block,
};

bool
virtio_gpu_base_device_realize(DeviceState *qdev,
                               VirtIOHandleOutput ctrl_cb,
                               VirtIOHandleOutput cursor_cb,
                               Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(qdev);
    VirtIOGPUBase *g = VIRTIO_GPU_BASE(qdev);
    Error *local_err = NULL;
    int i;

    if (g->conf.max_outputs > VIRTIO_GPU_MAX_SCANOUTS) {
        error_setg(errp, "invalid max_outputs > %d", VIRTIO_GPU_MAX_SCANOUTS);
        return false;
    }

    g->use_virgl_renderer = false;
    if (virtio_gpu_virgl_enabled(g->conf)) {
        error_setg(&g->migration_blocker, "virgl is not yet migratable");
        migrate_add_blocker(g->migration_blocker, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            error_free(g->migration_blocker);
            return false;
        }
    }

    g->virtio_config.num_scanouts = cpu_to_le32(g->conf.max_outputs);
    virtio_init(VIRTIO_DEVICE(g), "virtio-gpu", VIRTIO_ID_GPU,
                sizeof(struct virtio_gpu_config));

    if (virtio_gpu_virgl_enabled(g->conf)) {
        /* use larger control queue in 3d mode */
        virtio_add_queue(vdev, 256, ctrl_cb);
        virtio_add_queue(vdev, 16, cursor_cb);
    } else {
        virtio_add_queue(vdev, 64, ctrl_cb);
        virtio_add_queue(vdev, 16, cursor_cb);
    }

    g->enabled_output_bitmask = 1;

    g->req_state[0].width = g->conf.xres;
    g->req_state[0].height = g->conf.yres;

    for (i = 0; i < g->conf.max_outputs; i++) {
        g->scanout[i].con =
            graphic_console_init(DEVICE(g), i, &virtio_gpu_ops, g);
        if (i > 0) {
            dpy_gfx_replace_surface(g->scanout[i].con, NULL);
        }
    }

    return true;
}

static uint64_t
virtio_gpu_base_get_features(VirtIODevice *vdev, uint64_t features,
                             Error **errp)
{
    VirtIOGPUBase *g = VIRTIO_GPU_BASE(vdev);

    if (virtio_gpu_virgl_enabled(g->conf)) {
        features |= (1 << VIRTIO_GPU_F_VIRGL);
    }
    if (virtio_gpu_edid_enabled(g->conf)) {
        features |= (1 << VIRTIO_GPU_F_EDID);
    }
    if (virtio_gpu_shared_enabled(g->conf)) {
        features |= (1 << VIRTIO_GPU_F_RESOURCE_SHARED);
    }

    return features;
}

static void
virtio_gpu_base_set_features(VirtIODevice *vdev, uint64_t features)
{
    static const uint32_t virgl = (1 << VIRTIO_GPU_F_VIRGL);
    VirtIOGPUBase *g = VIRTIO_GPU_BASE(vdev);

    g->use_virgl_renderer = ((features & virgl) == virgl);
    trace_virtio_gpu_features(g->use_virgl_renderer);
}

static void
virtio_gpu_base_device_unrealize(DeviceState *qdev, Error **errp)
{
    VirtIOGPUBase *g = VIRTIO_GPU_BASE(qdev);

    if (g->migration_blocker) {
        migrate_del_blocker(g->migration_blocker);
        error_free(g->migration_blocker);
    }
}

static void
virtio_gpu_base_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    vdc->unrealize = virtio_gpu_base_device_unrealize;
    vdc->get_features = virtio_gpu_base_get_features;
    vdc->set_features = virtio_gpu_base_set_features;

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->hotpluggable = false;
}

static const TypeInfo virtio_gpu_base_info = {
    .name = TYPE_VIRTIO_GPU_BASE,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOGPUBase),
    .class_size = sizeof(VirtIOGPUBaseClass),
    .class_init = virtio_gpu_base_class_init,
    .abstract = true
};

static void
virtio_register_types(void)
{
    type_register_static(&virtio_gpu_base_info);
}

type_init(virtio_register_types)

QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_ctrl_hdr)                != 24);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_update_cursor)           != 56);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resource_unref)          != 32);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resource_create_2d)      != 40);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_set_scanout)             != 48);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resource_flush)          != 48);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_transfer_to_host_2d)     != 56);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_mem_entry)               != 16);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resource_attach_backing) != 32);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resource_detach_backing) != 32);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resp_display_info)       != 408);

QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_transfer_host_3d)        != 72);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resource_create_3d)      != 72);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_ctx_create)              != 96);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_ctx_destroy)             != 24);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_ctx_resource)            != 32);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_cmd_submit)              != 32);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_get_capset_info)         != 32);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resp_capset_info)        != 40);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_get_capset)              != 32);
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_resp_capset)             != 24);
