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
#include "qemu/units.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "ui/console.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "trace.h"

#ifdef CONFIG_LINUX

#include "cpu.h"
#include "exec/address-spaces.h"
#include "exec/ram_addr.h"

#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include "standard-headers/linux/udmabuf.h"

static void virtio_gpu_create_udmabuf(struct virtio_gpu_simple_resource *res)
{
    struct udmabuf_create_list *list;
    MemoryRegion *mr;
    FlatView *fv;
    int memfd, udmabuf, i;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0) {
        return;
    }

    list = g_malloc0(sizeof(struct udmabuf_create_list) +
                     sizeof(struct udmabuf_create_item) * res->iov_cnt);

    for (i = 0; i < res->iov_cnt; i++) {
        uint64_t a = res->addrs[i];
        uint32_t l = res->iov[i].iov_len;
        hwaddr xlat, len = l;

        rcu_read_lock();
        fv = address_space_to_flatview(&address_space_memory);
        mr = flatview_translate(fv, a, &xlat, &len, true,
                                MEMTXATTRS_UNSPECIFIED);
        memfd = mr->ram_block->fd;
        rcu_read_unlock();

        list->list[i].memfd  = memfd;
        list->list[i].offset = xlat;
        list->list[i].size   = len;
    }
    list->count = res->iov_cnt;
    list->flags = UDMABUF_FLAGS_CLOEXEC;

    res->dmabuf_fd = ioctl(udmabuf, UDMABUF_CREATE_LIST, list);
    if (res->dmabuf_fd < 0) {
        warn_report("%s: UDMABUF_CREATE_LIST: %s", __func__,
                    strerror(errno));
    }
    g_free(list);
}

static void virtio_gpu_remap_udmabuf(struct virtio_gpu_simple_resource *res)
{
    if (res->blob) {
        res->remapsz = res->blob_size;
    } else {
        res->remapsz = res->stride * res->height;
    }
    res->remapsz = QEMU_ALIGN_UP(res->remapsz, getpagesize());
    res->remapped = mmap(NULL, res->remapsz, PROT_READ,
                         MAP_SHARED, res->dmabuf_fd, 0);
    if (res->remapped == MAP_FAILED) {
        warn_report("%s: dmabuf mmap failed: %s", __func__,
                    strerror(errno));
        res->remapped = NULL;
    }
}

static void virtio_gpu_destroy_udmabuf(struct virtio_gpu_simple_resource *res)
{
    if (res->remapped) {
        munmap(res->remapped, res->remapsz);
        res->remapped = NULL;
    }
    if (res->dmabuf_fd >= 0) {
        close(res->dmabuf_fd);
        res->dmabuf_fd = -1;
    }
}

bool virtio_gpu_have_udmabuf(void)
{
    int udmabuf;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0)
        return false;

    /*
     * FIXME: should check ram is memfd-backed?
     */
    return true;
}

void virtio_gpu_init_udmabuf(struct virtio_gpu_simple_resource *res)
{
    uint32_t pformat = virtio_gpu_get_pixman_format(res->format);
    void *pdata;

    fprintf(stderr, "%s: id %d, %dx%d, %d iov(s), %s\n", __func__,
            res->resource_id, res->width, res->height, res->iov_cnt,
            res->iov_cnt == 1 ? "direct" : "dmabuf");

    res->dmabuf_fd = -1;
    if (res->iov_cnt == 1) {
        pdata = res->iov[0].iov_base;
    } else {
        virtio_gpu_create_udmabuf(res);
        if (res->dmabuf_fd < 0) {
            return;
        }
        virtio_gpu_remap_udmabuf(res);
        if (!res->remapped) {
            return;
        }
        pdata = res->remapped;
    }

    if (res->type == VIRTIO_GPU_RES_TYPE_SHARED) {
        res->blob = pdata;
    } else {
        qemu_pixman_image_unref(res->image);
        res->image = pixman_image_create_bits(pformat,
                                              res->width, res->height,
                                              pdata, res->stride);
    }
}

void virtio_gpu_fini_udmabuf(struct virtio_gpu_simple_resource *res)
{
    if (res->remapped) {
        fprintf(stderr, "%s: id %d, destroy dmabuf\n", __func__, res->resource_id);
        virtio_gpu_destroy_udmabuf(res);
    }
}

#else

bool virtio_gpu_have_udmabuf(void)
{
    /* nothing (stub) */
    return false;
}

void virtio_gpu_init_udmabuf(struct virtio_gpu_simple_resource *res)
{
    /* nothing (stub) */
}

void virtio_gpu_fini_udmabuf(struct virtio_gpu_simple_resource *res)
{
    /* nothing (stub) */
}

#endif
