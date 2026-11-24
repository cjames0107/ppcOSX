/*
 * QEMU Uninorth PCI host (for all Mac99 and newer machines)
 *
 * Copyright (c) 2006 Fabrice Bellard
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

#ifndef UNINORTH_H
#define UNINORTH_H

#include "hw/pci/pci_host.h"
#include "qom/object.h"

/* UniNorth version */
#define UNINORTH_VERSION_10A    0x7

#define TYPE_UNI_NORTH_PCI_HOST_BRIDGE "uni-north-pci-pcihost"
#define TYPE_UNI_NORTH_AGP_HOST_BRIDGE "uni-north-agp-pcihost"
#define TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE "uni-north-internal-pci-pcihost"
#define TYPE_U3_AGP_HOST_BRIDGE "u3-agp-pcihost"

typedef struct UNINHostState UNINHostState;
DECLARE_INSTANCE_CHECKER(UNINHostState, UNI_NORTH_PCI_HOST_BRIDGE,
                         TYPE_UNI_NORTH_PCI_HOST_BRIDGE)
DECLARE_INSTANCE_CHECKER(UNINHostState, UNI_NORTH_AGP_HOST_BRIDGE,
                         TYPE_UNI_NORTH_AGP_HOST_BRIDGE)
DECLARE_INSTANCE_CHECKER(UNINHostState, UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE,
                         TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE)
DECLARE_INSTANCE_CHECKER(UNINHostState, U3_AGP_HOST_BRIDGE,
                         TYPE_U3_AGP_HOST_BRIDGE)

struct UNINHostState {
    PCIHostState parent_obj;

    uint32_t ofw_addr;
    qemu_irq irqs[4];
    MemoryRegion pci_mmio;
    MemoryRegion pci_hole;
    MemoryRegion pci_io;
};

/* UniNorth AGP host bridge configuration registers */
#define UNI_N_AGP_CAP               0x80
#define UNI_N_CFG_GART_BASE         0x8c
#define UNI_N_CFG_AGP_BASE          0x90
#define UNI_N_CFG_GART_CTRL         0x94
#define UNI_N_CFG_INTERNAL_STATUS   0x98
#define UNI_N_CFG_GART_INVAL        0x00000001
#define UNI_N_CFG_GART_ENABLE       0x00000100
#define UNI_N_CFG_GART_2XRESET      0x00010000

bool unin_agp_translate(PCIBus *bus, uint64_t addr, uint64_t *out);

struct UNINState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
};

#define TYPE_UNI_NORTH "uni-north"
OBJECT_DECLARE_SIMPLE_TYPE(UNINState, UNI_NORTH)

#endif /* UNINORTH_H */
