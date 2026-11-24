/*
 * PowerMac NVRAM emulation
 *
 * Copyright (c) 2005-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
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
#include "qapi/error.h"
#include "hw/nvram/chrp_nvram.h"
#include "hw/nvram/mac_nvram.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "system/system.h"
#include "migration/vmstate.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "trace.h"
#include <zlib.h> /* for adler32 */

#define DEF_SYSTEM_SIZE 0xc10

#define CORE99_FLASH_BLOCK      0x2000

static void macio_nvram_store(MacIONVRAMState *s, hwaddr addr, hwaddr len)
{
    if (s->blk) {
        if (blk_pwrite(s->blk, addr, len, &s->data[addr], 0) < 0) {
            error_report("%s: write of NVRAM data to backing store failed",
                         blk_name(s->blk));
        }
    }
}

/*
 * Core99 flash commands, as Mac OS X's AppleCore99NVRAM issues them:
 *   erase block:  0x20, then 0xD0 to the same address (8 KiB block)
 *   program byte: 0x40 (or 0x10), then the data to the same address
 *   then status reads (bit 7 = ready, bits 3-5 = errors) until 0xFF
 *   returns the chip to reading data.
 * OpenBIOS treats the NVRAM as plain memory and writes each byte once, in
 * order, so a write is a plain store unless it completes one of the
 * two-write sequences above at the address just written; the setup byte
 * itself is stored and then replaced, which is harmless.
 */
static bool macio_nvram_flash_write(MacIONVRAMState *s, hwaddr addr,
                                    uint8_t value)
{
    bool repeat = s->flash_last_addr == addr;
    uint8_t last = s->flash_last_value;

    s->flash_last_addr = addr;
    s->flash_last_value = value;
    if (s->flash_status) {
        s->flash_status = false;
        if (value == 0xff || value == 0x70 || value == 0x50) {
            /* read array / read status / clear status: commands only */
            s->flash_status = value == 0x70;
            s->flash_last_addr = -1;
            return true;
        }
    }
    if (repeat && last == 0x20 && value == 0xd0) {
        hwaddr base = addr & ~(hwaddr)(CORE99_FLASH_BLOCK - 1);
        memset(&s->data[base], 0xff, CORE99_FLASH_BLOCK);
        macio_nvram_store(s, base, CORE99_FLASH_BLOCK);
        s->flash_status = true;
        s->flash_last_addr = -1;
        return true;
    }
    if (repeat && (last == 0x40 || last == 0x10)) {
        s->data[addr] = value;
        macio_nvram_store(s, addr, 1);
        s->flash_status = true;
        s->flash_last_addr = -1;
        return true;
    }
    return false;
}

/* macio style NVRAM device */
static void macio_nvram_writeb(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    MacIONVRAMState *s = opaque;

    addr = (addr >> s->it_shift) & (s->size - 1);
    trace_macio_nvram_write(addr, value);
    if (s->flash && macio_nvram_flash_write(s, addr, value)) {
        return;
    }
    s->data[addr] = value;
    macio_nvram_store(s, addr, 1);
}

static uint64_t macio_nvram_readb(void *opaque, hwaddr addr,
                                  unsigned size)
{
    MacIONVRAMState *s = opaque;
    uint32_t value;

    addr = (addr >> s->it_shift) & (s->size - 1);
    value = s->flash && s->flash_status ? 0x80 : s->data[addr];
    trace_macio_nvram_read(addr, value);

    return value;
}

static const MemoryRegionOps macio_nvram_ops = {
    .read = macio_nvram_readb,
    .write = macio_nvram_writeb,
    .valid.min_access_size = 1,
    /*
     * A G5's kernel copies the flash out with 8-byte loads; refusing them
     * read the banks back empty, and Leopard's IODTNVRAM then walked a
     * zero-length partition forever, starving the rest of the system.
     */
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_BIG_ENDIAN,
};

static const VMStateDescription vmstate_macio_nvram = {
    .name = "macio_nvram",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(data, MacIONVRAMState, 0, NULL, size),
        VMSTATE_END_OF_LIST()
    }
};


static void pmac_core99_nvram_reset(MacIONVRAMState *s);

static void macio_nvram_reset(DeviceState *dev)
{
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    s->flash_status = false;
    s->flash_last_addr = -1;
    if (s->flash) {
        pmac_core99_nvram_reset(s);
    }
}

static void macio_nvram_realizefn(DeviceState *dev, Error **errp)
{
    SysBusDevice *d = SYS_BUS_DEVICE(dev);
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    s->data = g_malloc0(s->size);

    if (s->blk) {
        int64_t len = blk_getlength(s->blk);
        if (len < 0) {
            error_setg_errno(errp, -len,
                             "could not get length of nvram backing image");
            return;
        } else if (len != s->size) {
            error_setg_errno(errp, -len,
                             "invalid size nvram backing image");
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_pread(s->blk, 0, s->size, s->data, 0) < 0) {
            error_setg(errp, "can't read-nvram contents");
            return;
        }
    }

    memory_region_init_io(&s->mem, OBJECT(s), &macio_nvram_ops, s,
                          "macio-nvram", s->size << s->it_shift);
    sysbus_init_mmio(d, &s->mem);
}

static void macio_nvram_unrealizefn(DeviceState *dev)
{
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    g_free(s->data);
}

static const Property macio_nvram_properties[] = {
    DEFINE_PROP_UINT32("size", MacIONVRAMState, size, 0),
    DEFINE_PROP_UINT32("it_shift", MacIONVRAMState, it_shift, 0),
    DEFINE_PROP_BOOL("flash", MacIONVRAMState, flash, false),
    DEFINE_PROP_DRIVE("drive", MacIONVRAMState, blk),
};

static void macio_nvram_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = macio_nvram_realizefn;
    dc->unrealize = macio_nvram_unrealizefn;
    device_class_set_legacy_reset(dc, macio_nvram_reset);
    dc->vmsd = &vmstate_macio_nvram;
    device_class_set_props(dc, macio_nvram_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo macio_nvram_type_info = {
    .name = TYPE_MACIO_NVRAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MacIONVRAMState),
    .class_init = macio_nvram_class_init,
};

static void macio_nvram_register_types(void)
{
    type_register_static(&macio_nvram_type_info);
}

/* Set up a system OpenBIOS NVRAM partition */
static void pmac_format_nvram_partition_of(MacIONVRAMState *nvr, int off,
                                           int len)
{
    int sysp_end;

    /*
     * OpenBIOS nvram variables partition: all of the bank but a free
     * partition's header. OpenBIOS cannot grow its partition, and Mac OS X
     * 10.0's BootX gives up (back to Open Firmware) when it cannot store a
     * variable, which happened with long -prom-env Forth (a Rage 128 and
     * the sound chip) in the 0xc10 bytes this partition had.
     */
    sysp_end = chrp_nvram_create_system_partition(&nvr->data[off],
                                                  MAX(DEF_SYSTEM_SIZE, len - 16),
                                                  len) + off;

    /* Free space partition */
    chrp_nvram_create_free_partition(&nvr->data[sysp_end], len - sysp_end);
}

#define OSX_NVRAM_SIGNATURE     (0x5A)

/* Set up a Mac OS X NVRAM partition */
static void pmac_format_nvram_partition_osx(MacIONVRAMState *nvr, int off,
                                            int len)
{
    uint32_t start = off;
    ChrpNvramPartHdr *part_header;
    unsigned char *data = &nvr->data[start];

    /* empty partition */
    part_header = (ChrpNvramPartHdr *)data;
    part_header->signature = OSX_NVRAM_SIGNATURE;
    pstrcpy(part_header->name, sizeof(part_header->name), "wwwwwwwwwwww");

    chrp_nvram_finish_partition(part_header, len);

    /* Generation */
    stl_be_p(&data[20], 2);

    /* Adler32 checksum */
    stl_be_p(&data[16], adler32(1, &data[20], len - 20));
}

#define CORE99_NVRAM_HEADER_SIZE   32      /* bank header */
#define CORE99_NVRAM_COMMON_SIZE   0x800   /* Open Firmware variables */

/*
 * Format a Core99 flash bank the way Mac OS X's AppleCore99NVRAM and
 * IODTNVRAM read it: a 32-byte "nvram" header (signature 0x5A, CHRP
 * checksum, Adler-32 of bytes 20.. at 16, generation at 20), a "common"
 * partition holding the Open Firmware variables as name=value
 * strings, and the rest free (0x7F "wwwwwwwwwwww").
 *
 * IODTNVRAM finds the firmware variables only in a partition named
 * "common" and never creates one; without it every firmware variable
 * access fails (nvram(8) reports kIOReturnUnsupported), and Panther's
 * installer refuses every disk ("You cannot start up your computer using
 * this volume"). The -prom-env variables are copied in.
 */
static void pmac_format_nvram_core99_bank(uint8_t *data, int len,
                                          uint32_t generation)
{
    ChrpNvramPartHdr *part_header;
    uint8_t *common = data + CORE99_NVRAM_HEADER_SIZE;
    int end, i;

    memset(data, 0, len);

    part_header = (ChrpNvramPartHdr *)data;
    part_header->signature = OSX_NVRAM_SIGNATURE;
    pstrcpy(part_header->name, sizeof(part_header->name), "nvram");
    chrp_nvram_finish_partition(part_header, CORE99_NVRAM_HEADER_SIZE);

    /*
     * Real Macs use signature 0x70 here, but Mac OS X finds the partition
     * by name only, and OpenBIOS takes every 0x70 partition for its own
     * variables (it would write its settings over this one).
     */
    part_header = (ChrpNvramPartHdr *)common;
    part_header->signature = CHRP_NVPART_SYSTEM + 1;
    pstrcpy(part_header->name, sizeof(part_header->name), "common");
    end = sizeof(ChrpNvramPartHdr);
    for (i = 0; i < nb_prom_envs; i++) {
        int n = strlen(prom_envs[i]) + 1;
        if (end + n >= CORE99_NVRAM_COMMON_SIZE) {
            break;
        }
        memcpy(&common[end], prom_envs[i], n);
        end += n;
    }
    chrp_nvram_finish_partition(part_header, CORE99_NVRAM_COMMON_SIZE);

    part_header = (ChrpNvramPartHdr *)(common + CORE99_NVRAM_COMMON_SIZE);
    part_header->signature = CHRP_NVPART_FREE;
    memcpy(part_header->name, "wwwwwwwwwwww", 12);  /* all 12, no NUL */
    chrp_nvram_finish_partition(part_header, len - CORE99_NVRAM_HEADER_SIZE -
                                CORE99_NVRAM_COMMON_SIZE);

    stl_be_p(&data[20], generation);
    stl_be_p(&data[16], adler32(1, &data[20], len - 20));
}

static bool pmac_core99_bank_valid(const uint8_t *bank, int len)
{
    return bank[0] == OSX_NVRAM_SIGNATURE &&
           ldl_be_p(&bank[16]) == adler32(1, &bank[20], len - 20);
}

/*
 * At machine reset: Mac OS X saves into whichever bank is older, which can
 * be bank A, where OpenBIOS keeps its variables. OpenBIOS would then load
 * Mac OS X's ones (boot-command=mac-boot...) and stop at its prompt. So
 * move Mac OS X's newest bank to B and give bank A back to OpenBIOS.
 */
static void pmac_core99_nvram_reset(MacIONVRAMState *s)
{
    int half = s->size / 2;
    uint8_t *a = s->data, *b = s->data + half;
    bool va = pmac_core99_bank_valid(a, half);
    bool vb = pmac_core99_bank_valid(b, half);

    if (!va && !vb) {
        pmac_format_nvram_core99_bank(b, half, 2);
    } else if (va && (!vb || ldl_be_p(&a[20]) > ldl_be_p(&b[20]))) {
        memcpy(b, a, half);
    } else if (!va) {
        return;     /* already laid out: OpenBIOS in A, Mac OS X in B */
    }
    memset(a, 0, half);
    pmac_format_nvram_partition_of(s, 0, half);
    macio_nvram_store(s, 0, s->size);
}

/*
 * Set up a flat (byte per address) NewWorld NVRAM as two Core99 flash
 * banks of len / 2: bank A holds OpenBIOS's own variables, bank B is what
 * Mac OS X reads (it takes the bank with a valid header and the newest
 * generation, and writes the other one).
 */
void pmac_format_nvram_core99(MacIONVRAMState *nvr, int len)
{
    pmac_format_nvram_partition_of(nvr, 0, len / 2);
    pmac_format_nvram_core99_bank(&nvr->data[len / 2], len / 2, 2);
}

/* Set up NVRAM with OF and OSX partitions */
void pmac_format_nvram_partition(MacIONVRAMState *nvr, int len)
{
    /*
     * Mac OS X expects side "B" of the flash at the second half of NVRAM,
     * so we use half of the chip for OF and the other half for a free OSX
     * partition.
     */
    pmac_format_nvram_partition_of(nvr, 0, len / 2);
    pmac_format_nvram_partition_osx(nvr, len / 2, len / 2);
}
type_init(macio_nvram_register_types)
