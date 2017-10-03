/*
 * virtio-iommu device
 *
 * Copyright (c) 2017 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu-common.h"
#include "hw/virtio/virtio.h"
#include "sysemu/kvm.h"
#include "qapi-event.h"
#include "qemu/error-report.h"
#include "trace.h"

#include "standard-headers/linux/virtio_ids.h"
#include <linux/virtio_iommu.h>

#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "hw/virtio/virtio-iommu.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci.h"

/* Max size */
#define VIOMMU_DEFAULT_QUEUE_SIZE 256
#define VIOMMU_PROBE_SIZE 512

#define SUPPORTED_PROBE_PROPERTIES (\
    VIRTIO_IOMMU_PROBE_T_NONE | \
    VIRTIO_IOMMU_PROBE_T_RESV_MEM)

typedef struct viommu_as {
    uint32_t id;
    GTree *mappings;
    QLIST_HEAD(, viommu_dev) device_list;
} viommu_as;

typedef struct viommu_dev {
    uint32_t id;
    viommu_as *as;
    QLIST_ENTRY(viommu_dev) next;
    VirtIOIOMMU *viommu;
    GTree *reserved_regions;
} viommu_dev;

typedef struct viommu_interval {
    uint64_t low;
    uint64_t high;
} viommu_interval;

typedef struct viommu_mapping {
    uint64_t virt_addr;
    uint64_t phys_addr;
    uint64_t size;
    uint32_t flags;
} viommu_mapping;

typedef struct viommu_property_buffer {
    viommu_dev *dev;
    size_t filled;
    uint8_t *start;
    bool error;
} viommu_property_buffer;

static inline uint16_t virtio_iommu_get_sid(IOMMUDevice *dev)
{
    return PCI_BUILD_BDF(pci_bus_num(dev->bus), dev->devfn);
}

static gint interval_cmp(gconstpointer a, gconstpointer b, gpointer user_data)
{
    viommu_interval *inta = (viommu_interval *)a;
    viommu_interval *intb = (viommu_interval *)b;

    if (inta->high <= intb->low) {
        return -1;
    } else if (intb->high <= inta->low) {
        return 1;
    } else {
        return 0;
    }
}

static void virtio_iommu_notify_map(IOMMUMemoryRegion *mr, hwaddr iova,
                                    hwaddr paddr, hwaddr size)
{
    IOMMUTLBEntry entry;

    entry.target_as = &address_space_memory;
    entry.addr_mask = size - 1;

    entry.iova = iova;
    trace_virtio_iommu_notify_map(mr->parent_obj.name, iova, paddr, size);
    entry.perm = IOMMU_RW;
    entry.translated_addr = paddr;

    memory_region_notify_iommu(mr, entry);
}

static void virtio_iommu_notify_unmap(IOMMUMemoryRegion *mr, hwaddr iova,
                                      hwaddr paddr, hwaddr size)
{
    IOMMUTLBEntry entry;

    entry.target_as = &address_space_memory;
    entry.addr_mask = size - 1;

    entry.iova = iova;
    trace_virtio_iommu_notify_unmap(mr->parent_obj.name, iova, paddr, size);
    entry.perm = IOMMU_NONE;
    entry.translated_addr = 0;

    memory_region_notify_iommu(mr, entry);
}

static gboolean virtio_iommu_mapping_unmap(gpointer key, gpointer value,
                                         gpointer data)
{
    viommu_mapping *mapping = (viommu_mapping *) value;
    IOMMUMemoryRegion *mr = (IOMMUMemoryRegion *) data;

    virtio_iommu_notify_unmap(mr, mapping->virt_addr, 0, mapping->size);

    return false;
}

static gboolean virtio_iommu_mapping_map(gpointer key, gpointer value,
                                          gpointer data)
{
    viommu_mapping *mapping = (viommu_mapping *) value;
    IOMMUMemoryRegion *mr = (IOMMUMemoryRegion *) data;

    virtio_iommu_notify_map(mr, mapping->virt_addr, mapping->phys_addr,
                            mapping->size);

    return false;
}

static void virtio_iommu_detach_dev_from_as(viommu_dev *dev)
{
    VirtioIOMMUNotifierNode *node;
    VirtIOIOMMU *s = dev->viommu;
    viommu_as *as = dev->as;

    QLIST_FOREACH(node, &s->notifiers_list, next) {
        if (dev->id == node->iommu_dev->devfn) {
            g_tree_foreach(as->mappings, virtio_iommu_mapping_unmap,
                           &node->iommu_dev->iommu_mr);
        }
    }
    QLIST_REMOVE(dev, next);
    dev->as = NULL;
}

static viommu_dev *virtio_iommu_get_dev(VirtIOIOMMU *s, uint32_t devid)
{
    viommu_dev *dev;

    dev = g_tree_lookup(s->devices, GUINT_TO_POINTER(devid));
    if (dev) {
        return dev;
    }
    dev = g_malloc0(sizeof(*dev));
    dev->id = devid;
    dev->viommu = s;
    trace_virtio_iommu_get_dev(devid);
    g_tree_insert(s->devices, GUINT_TO_POINTER(devid), dev);
    dev->reserved_regions = g_tree_new_full((GCompareDataFunc)interval_cmp,
                                            NULL, (GDestroyNotify)g_free,
                                            (GDestroyNotify)g_free);
    return dev;
}

static void virtio_iommu_put_dev(gpointer data)
{
    viommu_dev *dev = (viommu_dev *)data;
    viommu_as *as = dev->as;

    if (as) {
        virtio_iommu_detach_dev_from_as(dev);
        g_tree_unref(as->mappings);
    }

    trace_virtio_iommu_put_dev(dev->id);
    g_tree_destroy(dev->reserved_regions);
    g_free(dev);
}

static viommu_as *virtio_iommu_get_as(VirtIOIOMMU *s, uint32_t asid)
{
    viommu_as *as;

    as = g_tree_lookup(s->address_spaces, GUINT_TO_POINTER(asid));
    if (as) {
        return as;
    }
    as = g_malloc0(sizeof(*as));
    as->id = asid;
    as->mappings = g_tree_new_full((GCompareDataFunc)interval_cmp,
                                   NULL, (GDestroyNotify)g_free,
                                   (GDestroyNotify)g_free);
    g_tree_insert(s->address_spaces, GUINT_TO_POINTER(asid), as);
    QLIST_INIT(&as->device_list);
    trace_virtio_iommu_get_as(asid);
    return as;
}

static void virtio_iommu_put_as(gpointer data)
{
    viommu_as *as = (viommu_as *)data;
    viommu_dev *iter, *tmp;

    QLIST_FOREACH_SAFE(iter, &as->device_list, next, tmp) {
        virtio_iommu_detach_dev_from_as(iter);
    }
    g_tree_destroy(as->mappings);
    trace_virtio_iommu_put_as(as->id);
    g_free(as);
}

static AddressSpace *virtio_iommu_find_add_as(PCIBus *bus, void *opaque,
                                              int devfn)
{
    VirtIOIOMMU *s = opaque;
    uintptr_t key = (uintptr_t)bus;
    IOMMUPciBus *sbus = g_hash_table_lookup(s->as_by_busptr, &key);
    IOMMUDevice *sdev;

    if (!sbus) {
        uintptr_t *new_key = g_malloc(sizeof(*new_key));

        *new_key = (uintptr_t)bus;
        sbus = g_malloc0(sizeof(IOMMUPciBus) +
                         sizeof(IOMMUDevice *) * IOMMU_PCI_DEVFN_MAX);
        sbus->bus = bus;
        g_hash_table_insert(s->as_by_busptr, new_key, sbus);
    }

    sdev = sbus->pbdev[devfn];
    if (!sdev) {
        char *name = g_strdup_printf("%s-%d-%d",
                                     TYPE_VIRTIO_IOMMU_MEMORY_REGION,
                                     pci_bus_num(bus), devfn);
        sdev = sbus->pbdev[devfn] = g_malloc0(sizeof(IOMMUDevice));

        sdev->viommu = s;
        sdev->bus = bus;
        sdev->devfn = devfn;

        virtio_iommu_get_dev(s, PCI_BUILD_BDF(pci_bus_num(bus), devfn));

        trace_virtio_iommu_init_iommu_mr(name);

        memory_region_init_iommu(&sdev->iommu_mr, sizeof(sdev->iommu_mr),
                                 TYPE_VIRTIO_IOMMU_MEMORY_REGION,
                                 OBJECT(s), name,
                                 UINT64_MAX);
        address_space_init(&sdev->as,
                           MEMORY_REGION(&sdev->iommu_mr), TYPE_VIRTIO_IOMMU);
    }

    return &sdev->as;

}

static void virtio_iommu_init_as(VirtIOIOMMU *s)
{
    PCIBus *pcibus = pci_find_primary_bus();

    if (pcibus) {
        pci_setup_iommu(pcibus, virtio_iommu_find_add_as, s);
    } else {
        error_report("No PCI bus, virtio-iommu is not registered");
    }
}


static int virtio_iommu_attach(VirtIOIOMMU *s,
                               struct virtio_iommu_req_attach *req)
{
    uint32_t asid = le32_to_cpu(req->address_space);
    uint32_t devid = le32_to_cpu(req->device);
    uint32_t reserved = le32_to_cpu(req->reserved);
    VirtioIOMMUNotifierNode *node;
    viommu_as *as;
    viommu_dev *dev;

    trace_virtio_iommu_attach(asid, devid);

    if (reserved) {
        return VIRTIO_IOMMU_S_INVAL;
    }

    dev = virtio_iommu_get_dev(s, devid);
    if (dev->as) {
        /*
         * the device is already attached to an address space,
         * detach it first
         */
        virtio_iommu_detach_dev_from_as(dev);
    }

    as = virtio_iommu_get_as(s, asid);
    QLIST_INSERT_HEAD(&as->device_list, dev, next);

    dev->as = as;
    g_tree_ref(as->mappings);

    /* replay existing address space mappings on the associated mr */
    QLIST_FOREACH(node, &s->notifiers_list, next) {
        if (devid == node->iommu_dev->devfn) {
            g_tree_foreach(as->mappings, virtio_iommu_mapping_map,
                           &node->iommu_dev->iommu_mr);
        }
    }

    return VIRTIO_IOMMU_S_OK;
}

static int virtio_iommu_detach(VirtIOIOMMU *s,
                               struct virtio_iommu_req_detach *req)
{
    uint32_t devid = le32_to_cpu(req->device);
    uint32_t reserved = le32_to_cpu(req->reserved);
    viommu_dev *dev;

    if (reserved) {
        return VIRTIO_IOMMU_S_INVAL;
    }

    dev = g_tree_lookup(s->devices, GUINT_TO_POINTER(devid));
    if (!dev) {
        return VIRTIO_IOMMU_S_NOENT;
    }

    if (!dev->as) {
        return VIRTIO_IOMMU_S_INVAL;
    }

    virtio_iommu_detach_dev_from_as(dev);
    trace_virtio_iommu_detach(devid);
    return VIRTIO_IOMMU_S_OK;
}

static int virtio_iommu_map(VirtIOIOMMU *s,
                            struct virtio_iommu_req_map *req)
{
    uint32_t asid = le32_to_cpu(req->address_space);
    uint64_t phys_addr = le64_to_cpu(req->phys_addr);
    uint64_t virt_addr = le64_to_cpu(req->virt_addr);
    uint64_t size = le64_to_cpu(req->size);
    uint32_t flags = le32_to_cpu(req->flags);
    viommu_as *as;
    viommu_interval *interval;
    viommu_mapping *mapping;
    VirtioIOMMUNotifierNode *node;
    viommu_dev *dev;

    interval = g_malloc0(sizeof(*interval));

    interval->low = virt_addr;
    interval->high = virt_addr + size - 1;

    as = g_tree_lookup(s->address_spaces, GUINT_TO_POINTER(asid));
    if (!as) {
        return VIRTIO_IOMMU_S_NOENT;
    }

    mapping = g_tree_lookup(as->mappings, (gpointer)interval);
    if (mapping) {
        g_free(interval);
        return VIRTIO_IOMMU_S_INVAL;
    }

    trace_virtio_iommu_map(asid, phys_addr, virt_addr, size, flags);

    mapping = g_malloc0(sizeof(*mapping));
    mapping->virt_addr = virt_addr;
    mapping->phys_addr = phys_addr;
    mapping->size = size;
    mapping->flags = flags;

    g_tree_insert(as->mappings, interval, mapping);

    /* All devices in an address-space share mapping */
    QLIST_FOREACH(node, &s->notifiers_list, next) {
        QLIST_FOREACH(dev, &as->device_list, next) {
            if (dev->id == node->iommu_dev->devfn) {
                virtio_iommu_notify_map(&node->iommu_dev->iommu_mr,
                                        virt_addr, phys_addr, size);
            }
        }
    }

    return VIRTIO_IOMMU_S_OK;
}

static void virtio_iommu_remove_mapping(VirtIOIOMMU *s, viommu_as *as,
                                        viommu_interval *interval)
{
    VirtioIOMMUNotifierNode *node;
    viommu_dev *dev;

    g_tree_remove(as->mappings, (gpointer)(interval));
    QLIST_FOREACH(node, &s->notifiers_list, next) {
        QLIST_FOREACH(dev, &as->device_list, next) {
            if (dev->id == node->iommu_dev->devfn) {
                virtio_iommu_notify_unmap(&node->iommu_dev->iommu_mr,
                                          interval->low, 0,
                                          interval->high - interval->low + 1);
            }
        }
    }
}

static int virtio_iommu_unmap(VirtIOIOMMU *s,
                              struct virtio_iommu_req_unmap *req)
{
    uint32_t asid = le32_to_cpu(req->address_space);
    uint64_t virt_addr = le64_to_cpu(req->virt_addr);
    uint64_t size = le64_to_cpu(req->size);
    viommu_mapping *mapping;
    viommu_interval interval;
    viommu_as *as;

    trace_virtio_iommu_unmap(asid, virt_addr, size);

    as = g_tree_lookup(s->address_spaces, GUINT_TO_POINTER(asid));
    if (!as) {
        error_report("%s: no as", __func__);
        return VIRTIO_IOMMU_S_NOENT;
    }
    interval.low = virt_addr;
    interval.high = virt_addr + size - 1;

    mapping = g_tree_lookup(as->mappings, (gpointer)(&interval));

    while (mapping) {
        viommu_interval current;
        uint64_t low  = mapping->virt_addr;
        uint64_t high = mapping->virt_addr + mapping->size - 1;

        current.low = low;
        current.high = high;

        if (low == interval.low && size >= mapping->size) {
            virtio_iommu_remove_mapping(s, as, &current);
            interval.low = high + 1;
            trace_virtio_iommu_unmap_left_interval(current.low, current.high,
                interval.low, interval.high);
        } else if (high == interval.high && size >= mapping->size) {
            trace_virtio_iommu_unmap_right_interval(current.low, current.high,
                interval.low, interval.high);
            virtio_iommu_remove_mapping(s, as, &current);
            interval.high = low - 1;
        } else if (low > interval.low && high < interval.high) {
            trace_virtio_iommu_unmap_inc_interval(current.low, current.high);
            virtio_iommu_remove_mapping(s, as, &current);
        } else {
            break;
        }
        if (interval.low >= interval.high) {
            return VIRTIO_IOMMU_S_OK;
        } else {
            mapping = g_tree_lookup(as->mappings, (gpointer)(&interval));
        }
    }

    if (mapping) {
        error_report("****** %s: Unmap 0x%"PRIx64" size=0x%"PRIx64
                     " from 0x%"PRIx64" size=0x%"PRIx64" is not supported",
                     __func__, interval.low, size,
                     mapping->virt_addr, mapping->size);
    } else {
        return VIRTIO_IOMMU_S_OK;
    }

    return VIRTIO_IOMMU_S_INVAL;
}

static gboolean virtio_iommu_fill_resv_mem_prop(gpointer key,
                                                gpointer value,
                                                gpointer data)
{
    struct virtio_iommu_probe_resv_mem *resv =
        (struct virtio_iommu_probe_resv_mem *)value;
    struct virtio_iommu_probe_property *prop;
    struct virtio_iommu_probe_resv_mem *current;
    viommu_property_buffer *bufstate = (viommu_property_buffer *)data;
    size_t size = sizeof(*resv), total_size;

    total_size = size + 4;

    if (bufstate->filled >= VIOMMU_PROBE_SIZE) {
        bufstate->error = true;
        return true;
    }
    prop = (struct virtio_iommu_probe_property *)
                (bufstate->start + bufstate->filled);
    prop->type = cpu_to_le16(VIRTIO_IOMMU_PROBE_T_RESV_MEM) &
                    VIRTIO_IOMMU_PROBE_T_MASK;
    prop->length = size;

    current = (struct virtio_iommu_probe_resv_mem *)prop->value;
    *current = *resv;
    bufstate->filled += total_size;
    trace_virtio_iommu_fill_resv_property(bufstate->dev->id,
                                          resv->subtype, resv->addr,
                                          resv->size, resv->flags,
                                          bufstate->filled);
    return false;
}

static int virtio_iommu_fill_none_prop(viommu_property_buffer *bufstate)
{
    struct virtio_iommu_probe_property *prop;

    prop = (struct virtio_iommu_probe_property *)
                (bufstate->start + bufstate->filled);
    prop->type = cpu_to_le16(VIRTIO_IOMMU_PROBE_T_NONE)
                    & VIRTIO_IOMMU_PROBE_T_MASK;
    prop->length = 0;
    bufstate->filled += 4;
    trace_virtio_iommu_fill_none_property(bufstate->dev->id);
    return 0;
}

static int virtio_iommu_fill_property(int devid, int type,
                                      viommu_property_buffer *bufstate)
{
    int ret = -ENOSPC;

    if (bufstate->filled + 4 >= VIOMMU_PROBE_SIZE) {
        bufstate->error = true;
        goto out;
    }

    switch (type) {
    case VIRTIO_IOMMU_PROBE_T_NONE:
        ret = virtio_iommu_fill_none_prop(bufstate);
        break;
    case VIRTIO_IOMMU_PROBE_T_RESV_MEM:
    {
        viommu_dev *dev = bufstate->dev;

        g_tree_foreach(dev->reserved_regions,
                       virtio_iommu_fill_resv_mem_prop,
                       bufstate);
        if (!bufstate->error) {
            ret = 0;
        }
        break;
    }
    default:
        ret = -ENOENT;
        break;
    }
out:
    if (ret) {
        error_report("%s property of type=%d could not be filled (%d),"
                     " remaining size = 0x%lx",
                     __func__, type, ret, bufstate->filled);
    }
    return ret;
}

static int virtio_iommu_probe(VirtIOIOMMU *s,
                              struct virtio_iommu_req_probe *req,
                              uint8_t *buf)
{
    uint32_t devid = le32_to_cpu(req->device);
    int16_t prop_types = SUPPORTED_PROBE_PROPERTIES, type;
    viommu_property_buffer bufstate;
    viommu_dev *dev;
    int ret;

    dev = g_tree_lookup(s->devices, GUINT_TO_POINTER(devid));
    if (!dev) {
        return -EINVAL;
    }

    bufstate.start = buf;
    bufstate.filled = 0;
    bufstate.error = false;
    bufstate.dev = dev;

    while ((type = ctz32(prop_types)) != 32) {
        ret = virtio_iommu_fill_property(devid, 1 << type, &bufstate);
        if (ret) {
            break;
        }
        prop_types &= ~(1 << type);
    }
    virtio_iommu_fill_property(devid, VIRTIO_IOMMU_PROBE_T_NONE, &bufstate);

    return VIRTIO_IOMMU_S_OK;
}

#define get_payload_size(req) (\
sizeof((req)) - sizeof(struct virtio_iommu_req_tail))

static int virtio_iommu_handle_attach(VirtIOIOMMU *s,
                                      struct iovec *iov,
                                      unsigned int iov_cnt)
{
    struct virtio_iommu_req_attach req;
    size_t sz, payload_sz;

    payload_sz = get_payload_size(req);

    sz = iov_to_buf(iov, iov_cnt, 0, &req, payload_sz);
    if (sz != payload_sz) {
        return VIRTIO_IOMMU_S_INVAL;
    }
    return virtio_iommu_attach(s, &req);
}
static int virtio_iommu_handle_detach(VirtIOIOMMU *s,
                                      struct iovec *iov,
                                      unsigned int iov_cnt)
{
    struct virtio_iommu_req_detach req;
    size_t sz, payload_sz;

    payload_sz = get_payload_size(req);

    sz = iov_to_buf(iov, iov_cnt, 0, &req, payload_sz);
    if (sz != payload_sz) {
        return VIRTIO_IOMMU_S_INVAL;
    }
    return virtio_iommu_detach(s, &req);
}
static int virtio_iommu_handle_map(VirtIOIOMMU *s,
                                   struct iovec *iov,
                                   unsigned int iov_cnt)
{
    struct virtio_iommu_req_map req;
    size_t sz, payload_sz;

    payload_sz = get_payload_size(req);

    sz = iov_to_buf(iov, iov_cnt, 0, &req, payload_sz);
    if (sz != payload_sz) {
        return VIRTIO_IOMMU_S_INVAL;
    }
    return virtio_iommu_map(s, &req);
}
static int virtio_iommu_handle_unmap(VirtIOIOMMU *s,
                                     struct iovec *iov,
                                     unsigned int iov_cnt)
{
    struct virtio_iommu_req_unmap req;
    size_t sz, payload_sz;

    payload_sz = get_payload_size(req);

    sz = iov_to_buf(iov, iov_cnt, 0, &req, payload_sz);
    if (sz != payload_sz) {
        return VIRTIO_IOMMU_S_INVAL;
    }
    return virtio_iommu_unmap(s, &req);
}

static int virtio_iommu_handle_probe(VirtIOIOMMU *s,
                                     struct iovec *iov,
                                     unsigned int iov_cnt,
                                     uint8_t *buf)
{
    struct virtio_iommu_req_probe req;
    size_t sz, payload_sz;

    payload_sz = get_payload_size(req);

    sz = iov_to_buf(iov, iov_cnt, 0, &req, payload_sz);
    if (sz != payload_sz) {
        return VIRTIO_IOMMU_S_INVAL;
    }

    return virtio_iommu_probe(s, &req, buf);
}

static void virtio_iommu_handle_command(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOIOMMU *s = VIRTIO_IOMMU(vdev);
    VirtQueueElement *elem;
    struct virtio_iommu_req_head head;
    struct virtio_iommu_req_tail tail;
    unsigned int iov_cnt;
    struct iovec *iov;
    size_t sz;

    for (;;) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            return;
        }

        if (iov_size(elem->in_sg, elem->in_num) < sizeof(tail) ||
            iov_size(elem->out_sg, elem->out_num) < sizeof(head)) {
            virtio_error(vdev, "virtio-iommu erroneous head or tail");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            break;
        }

        iov_cnt = elem->out_num;
        iov = g_memdup(elem->out_sg, sizeof(struct iovec) * elem->out_num);
        sz = iov_to_buf(iov, iov_cnt, 0, &head, sizeof(head));
        if (sz != sizeof(head)) {
            tail.status = VIRTIO_IOMMU_S_UNSUPP;
        }
        qemu_mutex_lock(&s->mutex);
        switch (head.type) {
        case VIRTIO_IOMMU_T_ATTACH:
            tail.status = virtio_iommu_handle_attach(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_DETACH:
            tail.status = virtio_iommu_handle_detach(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_MAP:
            tail.status = virtio_iommu_handle_map(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_UNMAP:
            tail.status = virtio_iommu_handle_unmap(s, iov, iov_cnt);
            break;
        case VIRTIO_IOMMU_T_PROBE:
        {
            struct virtio_iommu_req_tail *ptail;
            uint8_t *buf = g_malloc0(s->config.probe_size + sizeof(tail));

            ptail = (struct virtio_iommu_req_tail *)buf + s->config.probe_size;
            ptail->status = virtio_iommu_handle_probe(s, iov, iov_cnt, buf);

            sz = iov_from_buf(elem->in_sg, elem->in_num, 0,
                              buf, s->config.probe_size + sizeof(tail));
            g_free(buf);
            assert(sz == s->config.probe_size + sizeof(tail));
            goto push;
        }
        default:
            tail.status = VIRTIO_IOMMU_S_UNSUPP;
        }

        sz = iov_from_buf(elem->in_sg, elem->in_num, 0,
                          &tail, sizeof(tail));
        assert(sz == sizeof(tail));

push:
        qemu_mutex_unlock(&s->mutex);
        virtqueue_push(vq, elem, sz);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_iommu_notify_flag_changed(IOMMUMemoryRegion *iommu_mr,
                                          IOMMUNotifierFlag old,
                                          IOMMUNotifierFlag new)
{
    IOMMUDevice *sdev = container_of(iommu_mr, IOMMUDevice, iommu_mr);
    VirtIOIOMMU *s = sdev->viommu;
    VirtioIOMMUNotifierNode *node = NULL;
    VirtioIOMMUNotifierNode *next_node = NULL;

    if (old == IOMMU_NOTIFIER_NONE) {
        trace_virtio_iommu_notify_flag_add(iommu_mr->parent_obj.name);
        node = g_malloc0(sizeof(*node));
        node->iommu_dev = sdev;
        QLIST_INSERT_HEAD(&s->notifiers_list, node, next);
        return;
    }

    /* update notifier node with new flags */
    QLIST_FOREACH_SAFE(node, &s->notifiers_list, next, next_node) {
        if (node->iommu_dev == sdev) {
            if (new == IOMMU_NOTIFIER_NONE) {
                trace_virtio_iommu_notify_flag_del(iommu_mr->parent_obj.name);
                QLIST_REMOVE(node, next);
                g_free(node);
            }
            return;
        }
    }
}

static IOMMUTLBEntry virtio_iommu_translate(IOMMUMemoryRegion *mr, hwaddr addr,
                                            IOMMUAccessFlags flag)
{
    IOMMUDevice *sdev = container_of(mr, IOMMUDevice, iommu_mr);
    VirtIOIOMMU *s = sdev->viommu;
    uint32_t sid;
    viommu_dev *dev;
    viommu_mapping *mapping;
    viommu_interval interval;

    interval.low = addr;
    interval.high = addr + 1;

    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr,
        .translated_addr = addr,
        .addr_mask = (1 << ctz32(s->config.page_size_mask)) - 1,
        .perm = flag,
    };

    sid = virtio_iommu_get_sid(sdev);

    trace_virtio_iommu_translate(mr->parent_obj.name, sid, addr, flag);
    qemu_mutex_lock(&s->mutex);

    dev = g_tree_lookup(s->devices, GUINT_TO_POINTER(sid));
    if (!dev) {
        error_report("%s sid=%d is not known!!", __func__, sid);
        goto unlock;
    }

    if (!dev->as) {
        error_report("%s sid=%d not attached to any address space",
                     __func__, sid);
        goto unlock;
    }

    mapping = g_tree_lookup(dev->as->mappings, (gpointer)(&interval));
    if (!mapping) {
        error_report("%s no mapping for 0x%"PRIx64" for sid=%d", __func__,
                     addr, sid);
        goto unlock;
    }

    if (((flag & IOMMU_RO) && !(mapping->flags & VIRTIO_IOMMU_MAP_F_READ)) ||
        ((flag & IOMMU_WO) && !(mapping->flags & VIRTIO_IOMMU_MAP_F_WRITE))) {
        error_report("Permission error on 0x%"PRIx64"(%d): allowed=%d",
                     addr, flag, mapping->flags);
        entry.perm = IOMMU_NONE;
        goto unlock;
    }
    entry.translated_addr = addr - mapping->virt_addr + mapping->phys_addr,
    trace_virtio_iommu_translate_out(addr, entry.translated_addr, sid);

unlock:
    qemu_mutex_unlock(&s->mutex);
    return entry;
}

static void virtio_iommu_set_page_size_mask(IOMMUMemoryRegion *mr,
                                            uint64_t page_size_mask)
{
    IOMMUDevice *sdev = container_of(mr, IOMMUDevice, iommu_mr);
    VirtIOIOMMU *s = sdev->viommu;

    s->config.page_size_mask &= page_size_mask;
    if (!s->config.page_size_mask) {
        error_setg(&error_fatal,
                   "No compatible page size between guest and host iommus");
    }

    trace_virtio_iommu_set_page_size_mask(mr->parent_obj.name, page_size_mask);
}

static void virtio_iommu_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOIOMMU *dev = VIRTIO_IOMMU(vdev);
    struct virtio_iommu_config *config = &dev->config;

    trace_virtio_iommu_get_config(config->page_size_mask,
                                  config->input_range.start,
                                  config->input_range.end,
                                  config->ioasid_bits,
                                  config->probe_size);
    memcpy(config_data, &dev->config, sizeof(struct virtio_iommu_config));
}

static void virtio_iommu_set_config(VirtIODevice *vdev,
                                      const uint8_t *config_data)
{
}

static uint64_t virtio_iommu_get_features(VirtIODevice *vdev, uint64_t f,
                                            Error **errp)
{
    VirtIOIOMMU *dev = VIRTIO_IOMMU(vdev);
    f |= dev->host_features;
    virtio_add_feature(&f, VIRTIO_RING_F_EVENT_IDX);
    virtio_add_feature(&f, VIRTIO_RING_F_INDIRECT_DESC);
    virtio_add_feature(&f, VIRTIO_IOMMU_F_INPUT_RANGE);
    virtio_add_feature(&f, VIRTIO_IOMMU_F_MAP_UNMAP);
    virtio_add_feature(&f, VIRTIO_IOMMU_F_PROBE);
    return f;
}

static void virtio_iommu_set_features(VirtIODevice *vdev, uint64_t val)
{
    trace_virtio_iommu_set_features(val);
}

static int virtio_iommu_post_load_device(void *opaque, int version_id)
{
    return 0;
}

static const VMStateDescription vmstate_virtio_iommu_device = {
    .name = "virtio-iommu-device",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = virtio_iommu_post_load_device,
    .fields = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

/*****************************
 * Hash Table
 *****************************/

static inline gboolean as_uint64_equal(gconstpointer v1, gconstpointer v2)
{
    return *((const uint64_t *)v1) == *((const uint64_t *)v2);
}

static inline guint as_uint64_hash(gconstpointer v)
{
    return (guint)*(const uint64_t *)v;
}

static gint int_cmp(gconstpointer a, gconstpointer b, gpointer user_data)
{
    uint ua = GPOINTER_TO_UINT(a);
    uint ub = GPOINTER_TO_UINT(b);
    return (ua > ub) - (ua < ub);
}

static gboolean virtio_iommu_remap(gpointer key, gpointer value, gpointer data)
{
    viommu_mapping *mapping = (viommu_mapping *) value;
    IOMMUMemoryRegion *mr = (IOMMUMemoryRegion *) data;

    trace_virtio_iommu_remap(mapping->virt_addr, mapping->phys_addr,
                             mapping->size);
    /* unmap previous entry and map again */
    virtio_iommu_notify_unmap(mr, mapping->virt_addr, 0, mapping->size);

    virtio_iommu_notify_map(mr, mapping->virt_addr, mapping->phys_addr,
                            mapping->size);
    return false;
}

static void virtio_iommu_replay(IOMMUMemoryRegion *mr, IOMMUNotifier *n)
{
    IOMMUDevice *sdev = container_of(mr, IOMMUDevice, iommu_mr);
    VirtIOIOMMU *s = sdev->viommu;
    uint32_t sid;
    viommu_dev *dev;

    sid = virtio_iommu_get_sid(sdev);

    qemu_mutex_lock(&s->mutex);

    dev = g_tree_lookup(s->devices, GUINT_TO_POINTER(sid));
    if (!dev || !dev->as) {
        goto unlock;
    }

    g_tree_foreach(dev->as->mappings, virtio_iommu_remap, mr);

unlock:
    qemu_mutex_unlock(&s->mutex);
}

static void virtio_iommu_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOIOMMU *s = VIRTIO_IOMMU(dev);

    QLIST_INIT(&s->notifiers_list);
    virtio_init(vdev, "virtio-iommu", VIRTIO_ID_IOMMU,
                sizeof(struct virtio_iommu_config));

    s->vq = virtio_add_queue(vdev, VIOMMU_DEFAULT_QUEUE_SIZE,
                             virtio_iommu_handle_command);

    s->config.page_size_mask = TARGET_PAGE_MASK;
    s->config.input_range.end = -1UL;
    s->config.probe_size = VIOMMU_PROBE_SIZE;

    qemu_mutex_init(&s->mutex);

    memset(s->as_by_bus_num, 0, sizeof(s->as_by_bus_num));
    s->as_by_busptr = g_hash_table_new_full(as_uint64_hash,
                                            as_uint64_equal,
                                            g_free, g_free);

    s->address_spaces = g_tree_new_full((GCompareDataFunc)int_cmp,
                                         NULL, NULL, virtio_iommu_put_as);
    s->devices = g_tree_new_full((GCompareDataFunc)int_cmp,
                                 NULL, NULL, virtio_iommu_put_dev);

    virtio_iommu_init_as(s);
}

static void virtio_iommu_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOIOMMU *s = VIRTIO_IOMMU(dev);

    g_tree_destroy(s->address_spaces);
    g_tree_destroy(s->devices);

    virtio_cleanup(vdev);
}

static void virtio_iommu_device_reset(VirtIODevice *vdev)
{
    trace_virtio_iommu_device_reset();
}

static void virtio_iommu_set_status(VirtIODevice *vdev, uint8_t status)
{
    trace_virtio_iommu_device_status(status);
}

static void virtio_iommu_instance_init(Object *obj)
{
}

static const VMStateDescription vmstate_virtio_iommu = {
    .name = "virtio-iommu",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_iommu_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_iommu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = virtio_iommu_properties;
    dc->vmsd = &vmstate_virtio_iommu;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_iommu_device_realize;
    vdc->unrealize = virtio_iommu_device_unrealize;
    vdc->reset = virtio_iommu_device_reset;
    vdc->get_config = virtio_iommu_get_config;
    vdc->set_config = virtio_iommu_set_config;
    vdc->get_features = virtio_iommu_get_features;
    vdc->set_features = virtio_iommu_set_features;
    vdc->set_status = virtio_iommu_set_status;
    vdc->vmsd = &vmstate_virtio_iommu_device;
}

static void virtio_iommu_memory_region_class_init(ObjectClass *klass,
                                                  void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = virtio_iommu_translate;
    imrc->set_page_size_mask = virtio_iommu_set_page_size_mask;
    imrc->notify_flag_changed = virtio_iommu_notify_flag_changed;
    imrc->replay = virtio_iommu_replay;
}

static const TypeInfo virtio_iommu_info = {
    .name = TYPE_VIRTIO_IOMMU,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOIOMMU),
    .instance_init = virtio_iommu_instance_init,
    .class_init = virtio_iommu_class_init,
};

static const TypeInfo virtio_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_VIRTIO_IOMMU_MEMORY_REGION,
    .class_init = virtio_iommu_memory_region_class_init,
};


static void virtio_register_types(void)
{
    type_register_static(&virtio_iommu_info);
    type_register_static(&virtio_iommu_memory_region_info);
}

type_init(virtio_register_types)
