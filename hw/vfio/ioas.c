/*
 * /dev/iommu specific functions used by VFIO devices
 *
 * Copyright Red Hat, Inc. 2022
 *
 * Authors:
 * <TODO>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#ifdef CONFIG_KVM
#include <linux/kvm.h>
#endif
#include <linux/vfio.h>
#include <linux/iommufd.h>

#include "hw/vfio/vfio-common.h"
#include "hw/vfio/vfio.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "exec/ram_addr.h"
#include "hw/hw.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/range.h"
#include "sysemu/kvm.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "trace.h"
#include "qapi/error.h"
#include "migration/migration.h"

static int
vfio_device_attach_ioas(VFIODevice *vbasedev, AddressSpace *as, Error **errp)
{
    VFIOContainer *container;
    VFIOAddressSpace *space;
    struct iommu_ioas_alloc ioas_alloc = {
                .size = sizeof(ioas_alloc),
                .flags = 0,
    };
    struct vfio_device_attach_ioas attach_ioas = {
                .argsz = sizeof(attach_ioas),
                .flags = 0,
    };
    int ret;

    error_report("%s %s", __func__, vbasedev->name);
    space = vfio_get_address_space(as);

    QLIST_FOREACH(container, &space->containers, next) {
        struct vfio_device_attach_ioas attach_ioas = {
                .argsz = sizeof(attach_ioas),
                .flags = 0,
        };

        attach_ioas.iommufd = space->iommufd;
        attach_ioas.ioas_id = container->ioas_id;
        ret = ioctl(vbasedev->devfd, VFIO_DEVICE_ATTACH_IOAS, &attach_ioas);
        if (!ret) {
            QLIST_INSERT_HEAD(&container->dev_list, vbasedev, container_next);
            return ret;
        }
    }

    /* Alloc a new container/ioas */
    ret = ioctl(space->iommufd, IOMMU_IOAS_ALLOC, &ioas_alloc);
    if (ret < 0) {
        error_report("Failed to alloc ioas (%s)", strerror(errno));
        return ret;
    } else {
        error_report("Allocated ioas=%d", ioas_alloc.out_ioas_id);
    }

    attach_ioas.iommufd = space->iommufd;
    attach_ioas.ioas_id = ioas_alloc.out_ioas_id;
    ret = ioctl(vbasedev->devfd, VFIO_DEVICE_ATTACH_IOAS, &attach_ioas);
    if (ret < 0) {
        error_report("%s Failed to attach %s to ioasid=%d (%s)",
                     __func__, vbasedev->name,
                     ioas_alloc.out_ioas_id, strerror(errno));
        return ret;
    }
    error_report("%s succesfully attached to ioas=%d",
                 __func__, attach_ioas.out_hwpt_id);

    container = g_malloc0(sizeof(*container));
    container->space = space;
    container->ioas_id = ioas_alloc.out_ioas_id;
    container->fd = 0;
    container->iommu_type = VFIO_IOMMUFD;
    container->error = NULL;
    container->dirty_pages_supported = false;
    container->dma_max_mappings = 0;
    QLIST_INIT(&container->giommu_list);
    QLIST_INIT(&container->hostwin_list);
    QLIST_INIT(&container->vrdl_list);
    QLIST_INIT(&container->dev_list);
    error_report("%s new container with ioas%d is finalized",
                 __func__, container->ioas_id);
    QLIST_INSERT_HEAD(&container->dev_list, vbasedev, container_next);

    vfio_host_win_add(container, 0, (hwaddr)-1, 4096);

    /*
     * TODO
     * vfio_ram_block_discard_disable && vfio_ram_block_discard_disable
     * container->iommu_type ?
     * get_iommu_info
     * dma_avail_ranges
     */

#if 0
    container->prereg_listener = vfio_prereg_listener;
    memory_listener_register(&container->prereg_listener,
                                     &address_space_memory);
    if (container->error) {
        memory_listener_unregister(&container->prereg_listener);
        ret = -1;
        error_propagate_prepend(errp, container->error,
                    "RAM memory listener initialization failed for prereg: ");
        return ret;
    }
#endif
    /* TODO KVM group */
    container->listener = vfio_memory_listener;
    memory_listener_register(&container->listener, container->space->as);
    if (container->error) {
        ret = -1;
        error_propagate_prepend(errp, container->error,
            "memory listener initialization failed: ");
        return ret;
    }


    container->initialized = true;

    return 0;
}

static int vfio_get_devicefd(const char *sysfs_path, Error **errp)
{
    int vfio_id = -1, ret = 0;
    char *path, *tmp;
    DIR *dir;
    struct dirent *dent;
    struct stat st;
    gchar *contents;
    gsize length;
    int major, minor;
    dev_t vfio_devt;

    path = g_strdup_printf("%s/vfio-device", sysfs_path);
    printf("path: %s, \n", path);
    if (stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "no such host device");
        error_prepend(errp, VFIO_MSG_PREFIX, path);
        return -ENOTTY;
    }

    dir = opendir(path);
    if (!dir) {
        ret = -ENOTTY;
        goto out;
    }

    while ((dent = readdir(dir))) {
        char *end_name;

        if (!strncmp(dent->d_name, "vfio", 4)) {
            vfio_id = strtol(dent->d_name + 4, &end_name, 10);
            break;
        }
    }

    printf("vfio_id: %d\n", vfio_id);
    if (vfio_id == -1) {
        ret = -ENOTTY;
        goto out;
    }

    /* check if the major:minor matches */
    tmp = g_strdup_printf("%s/%s/dev", path, dent->d_name);
    if (!g_file_get_contents(tmp, &contents, &length, NULL)) {
        error_report("failed to load \"%s\"", tmp);
        exit(1);
    }
    printf("tmp: %s, content: %s, len: %ld\n", tmp, contents, length);
    if (sscanf(contents, "%d:%d", &major, &minor) != 2) {
        error_report("failed to load \"%s\"", tmp);
        exit(1);
    }
    printf("%d, %d\n", major, minor);
    g_free(contents);
    g_free(tmp);

    tmp = g_strdup_printf("/dev/vfio/devices/vfio%d", vfio_id);
    if (stat(tmp, &st) < 0) {
        error_setg_errno(errp, errno, "no such vfio device");
        error_prepend(errp, VFIO_MSG_PREFIX, tmp);
        ret = -ENOTTY;
        goto out;
    }
    vfio_devt = makedev(major, minor);
    printf("vfio_devt: %lu, %lu\n", vfio_devt, st.st_rdev);
    if (st.st_rdev != vfio_devt) {
        ret = -EINVAL;
    } else {
        ret = qemu_open_old(tmp, O_RDWR);
    }
    g_free(tmp);

out:
    g_free(path);
    return ret;
}

int vfio_device_bind_iommufd(VFIODevice *vbasedev, AddressSpace *as,
                             Error **errp)
{
    VFIOAddressSpace *space;
    char path[64], devpath[32], line[32] = {};
    FILE *file;
    int ret, major, minor;
    struct vfio_device_bind_iommufd bind_data = {
                .argsz = sizeof(bind_data),
                .dev_cookie = 0xbeef,
    };

    vbasedev->devfd = vfio_get_devicefd(vbasedev->sysfsdev, errp);

    space = vfio_get_address_space(as);

    /* bind the device to the iommufd */
    bind_data.iommufd = space->iommufd;
    bind_data.flags = 0;
    ret = ioctl(vbasedev->devfd, VFIO_DEVICE_BIND_IOMMUFD, &bind_data);
    if (ret < 0) {
        error_report("%s failed to bind devfd=%d to iommufd=%d",
                     __func__, vbasedev->devfd, space->iommufd);
            return ret;
    }
    vbasedev->devid = bind_data.out_devid;
    error_report("%s succesfully bound devfd=%d to iommufd=%d: dev_id=%d",
                 __func__, vbasedev->devfd, space->iommufd, vbasedev->devid);

    ret = vfio_device_attach_ioas(vbasedev, as, errp);
    return ret;
}

int vfio_get_iommufd_device(VFIODevice *vbasedev, Error **errp)
{
    struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };
    int ret;

    ret = ioctl(vbasedev->devfd, VFIO_DEVICE_GET_INFO, &dev_info);
    if (ret) {
        error_setg_errno(errp, errno, "error getting device info");
        return ret;
    }

    vbasedev->fd = vbasedev->devfd;
    vbasedev->group = 0;

    vbasedev->num_irqs = dev_info.num_irqs;
    vbasedev->num_regions = dev_info.num_regions;
    vbasedev->flags = dev_info.flags;

    trace_vfio_get_device(vbasedev->name, dev_info.flags, dev_info.num_regions,
                          dev_info.num_irqs);

    vbasedev->reset_works = !!(dev_info.flags & VFIO_DEVICE_FLAGS_RESET);
    error_report("%s %s num_irqs=%d num_regions=%d", __func__,
                 vbasedev->name, vbasedev->num_irqs,
                 vbasedev->num_regions);
    return 0;
}
