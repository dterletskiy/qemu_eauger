/*
 * KVM-based ITS implementation for a GICv3-based system
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * Written by Pavel Fedin <p.fedin@samsung.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "kvm_arm.h"
#include "migration/migration.h"

#define TYPE_KVM_ARM_ITS "arm-its-kvm"
#define KVM_ARM_ITS(obj) OBJECT_CHECK(GICv3ITSState, (obj), TYPE_KVM_ARM_ITS)

static int kvm_its_send_msi(GICv3ITSState *s, uint32_t value, uint16_t devid)
{
    struct kvm_msi msi;

    if (unlikely(!s->translater_gpa_known)) {
        MemoryRegion *mr = &s->iomem_its_translation;
        MemoryRegionSection mrs;

        mrs = memory_region_find(mr, 0, 1);
        memory_region_unref(mrs.mr);
        s->gits_translater_gpa = mrs.offset_within_address_space + 0x40;
        s->translater_gpa_known = true;
    }

    msi.address_lo = extract64(s->gits_translater_gpa, 0, 32);
    msi.address_hi = extract64(s->gits_translater_gpa, 32, 32);
    msi.data = le32_to_cpu(value);
    msi.flags = KVM_MSI_VALID_DEVID;
    msi.devid = devid;
    memset(msi.pad, 0, sizeof(msi.pad));

    return kvm_vm_ioctl(kvm_state, KVM_SIGNAL_MSI, &msi);
}

/**
 * vm_change_state_handler - VM change state callback aiming at flushing
 * ITS tables into guest RAM
 *
 * The tables get flushed to guest RAM whenever the VM gets stopped.
 */
static void vm_change_state_handler(void *opaque, int running,
                                    RunState state)
{
    GICv3ITSState *s = (GICv3ITSState *)opaque;

    if (running) {
        return;
    }
    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_TABLES,
                      0, NULL, false);
}

static void kvm_arm_its_realize(DeviceState *dev, Error **errp)
{
    GICv3ITSState *s = ARM_GICV3_ITS_COMMON(dev);

    s->dev_fd = kvm_create_device(kvm_state, KVM_DEV_TYPE_ARM_VGIC_ITS, false);
    if (s->dev_fd < 0) {
        error_setg_errno(errp, -s->dev_fd, "error creating in-kernel ITS");
        return;
    }

    /* explicit init of the ITS */
    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_CTRL,
                      KVM_DEV_ARM_VGIC_CTRL_INIT, NULL, true);

    /* register the base address */
    kvm_arm_register_device(&s->iomem_its_cntrl, -1, KVM_DEV_ARM_VGIC_GRP_ADDR,
                            KVM_VGIC_ITS_ADDR_TYPE, s->dev_fd);

    gicv3_its_init_mmio(s, NULL);

    /*
     * Block migration of a KVM GICv3 ITS device: the API for saving and
     * restoring the state in the kernel is not yet available
     */
    if (!kvm_device_check_attr(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                               GITS_CTLR)) {
        error_setg(&s->migration_blocker, "This operating system kernel does "
                                          "not support vITS migration");
        migrate_add_blocker(s->migration_blocker);
    }

    kvm_msi_use_devid = true;
    kvm_gsi_direct_mapping = false;
    kvm_msi_via_irqfd_allowed = kvm_irqfds_enabled();

    qemu_add_vm_change_state_handler(vm_change_state_handler, s);
}

static void kvm_arm_its_init(Object *obj)
{
    GICv3ITSState *s = KVM_ARM_ITS(obj);

    object_property_add_link(obj, "parent-gicv3",
                             "kvm-arm-gicv3", (Object **)&s->gicv3,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_UNREF_ON_RELEASE,
                             &error_abort);
}

/**
 * kvm_arm_its_get - handles the saving of ITS registers.
 * ITS tables, being flushed into guest RAM needs to be saved before
 * the pre_save() callback, hence the migration state change notifiers
 */
static void kvm_arm_its_get(GICv3ITSState *s)
{
    uint64_t reg;
    int i;

    for (i = 0; i < 8; i++) {
        kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                          GITS_BASER + i * 8, &s->baser[i], false);
    }

    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CTLR, &reg, false);
    s->ctlr = extract64(reg, 0, 32);

    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CBASER, &s->cbaser, false);

    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CREADR, &s->creadr, false);

    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CWRITER, &s->cwriter, false);
}

/**
 * kvm_arm_its_put - Restore both the ITS registers and guest RAM tables
 * ITS tables, being flushed into guest RAM needs to be saved before
 * the pre_save() callback. The restoration order matters since there
 * are dependencies between register settings, as specified by the
 * architecture specification
 */
static void kvm_arm_its_put(GICv3ITSState *s)
{
    uint64_t reg;
    int i;

    /* must be written before GITS_CREADR since it resets this latter*/
    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CBASER, &s->cbaser, true);

    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CREADR, &s->creadr, true);

    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CWRITER, &s->cwriter, true);

    for (i = 0; i < 8; i++) {
        kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                          GITS_BASER + i * 8, &s->baser[i], true);
    }

    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_TABLES,
                      0, NULL, true);

    reg = s->ctlr;
    kvm_device_access(s->dev_fd, KVM_DEV_ARM_VGIC_GRP_ITS_REGS,
                      GITS_CTLR, &reg, true);
}

static void kvm_arm_its_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    GICv3ITSCommonClass *icc = ARM_GICV3_ITS_COMMON_CLASS(klass);

    dc->realize = kvm_arm_its_realize;
    icc->send_msi = kvm_its_send_msi;
    icc->pre_save = kvm_arm_its_get;
    icc->post_load = kvm_arm_its_put;
}

static const TypeInfo kvm_arm_its_info = {
    .name = TYPE_KVM_ARM_ITS,
    .parent = TYPE_ARM_GICV3_ITS_COMMON,
    .instance_size = sizeof(GICv3ITSState),
    .instance_init = kvm_arm_its_init,
    .class_init = kvm_arm_its_class_init,
};

static void kvm_arm_its_register_types(void)
{
    type_register_static(&kvm_arm_its_info);
}

type_init(kvm_arm_its_register_types)
