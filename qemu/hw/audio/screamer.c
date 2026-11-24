/*
 * Screamer sound chip (Apple part 343S0184), as on the davbus of
 * Power Macs and iMacs: an AWACS-compatible codec whose samples come and go
 * through two mac-io DBDMA channels.
 *
 * Mac OS 9 and Mac OS X (AppleScreamerAudio, in Apple02Audio up to 10.4)
 * drive it; Open Firmware describes it as /pci/mac-io/davbus/sound (made by
 * tools/sound-nvramrc.sh, as OpenBIOS has no such node).
 *
 * Registers, little-endian, 16 bytes apart:
 *   0x00 sound control   input/output subframes, sample rate (bits 8-10),
 *                        port-change interrupt
 *   0x10 codec control   a command for the codec: data in bits 0-11, the
 *                        register in bits 12-14 (7 reads one back: register
 *                        number in bits 1-3), bit 24 busy until done
 *   0x20 codec status    bit 22 valid; after a read-back, the value in bits
 *                        4-15; otherwise the revision (12-15), the maker
 *                        (8-11) and the input sense bits (0-3)
 *   0x30 clip count
 *   0x40 byte swap       samples are little-endian when 1
 *   0x50 frame count     sample frames played (the driver polls it for
 *                        the playback position; writing sets it)
 *
 * Output is 16-bit stereo at the rate the control register picks, given to
 * QEMU's audio backend as the backend asks for it: each DBDMA output
 * command completes once its bytes are played, which paces the guest.
 * Input records silence at the same rate.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ppc/mac_dbdma.h"
#include "hw/audio/screamer.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "trace.h"

#define REG_CONTROL     0x00
#define REG_CODEC_CTRL  0x10
#define REG_CODEC_STAT  0x20
#define REG_CLIP_COUNT  0x30
#define REG_BYTESWAP    0x40
#define REG_FRAME_COUNT 0x50

#define CTRL_RATE_SHIFT     8
#define CTRL_RATE_MASK      (7 << CTRL_RATE_SHIFT)

#define CODEC_BUSY          (1u << 24)
#define CODEC_ADDR_SHIFT    12
#define CODEC_ADDR_MASK     (7 << CODEC_ADDR_SHIFT)
#define CODEC_DATA_MASK     0xfff

#define STAT_VALID          (1u << 22)
#define STAT_REVISION_SHIFT 12
#define STAT_MAKER_SHIFT    8

/* codec register 1 */
#define R1_SPEAKER_MUTE     (1 << 7)    /* output C */
#define R1_HEADPHONE_MUTE   (1 << 9)    /* output A */

static const int screamer_rates[8] = {
    44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350
};

static int screamer_rate(ScreamerState *s)
{
    return screamer_rates[(s->control & CTRL_RATE_MASK) >> CTRL_RATE_SHIFT];
}

/* Attenuation in 1.5 dB steps (0-15) to QEMU's 0-255 volume */
static uint8_t screamer_atten(int steps)
{
    static const uint8_t table[16] = {
        255, 214, 180, 151, 127, 107, 90, 76, 64, 53, 45, 38, 32, 27, 22, 19
    };
    return table[steps & 15];
}

/* The volume of whichever output is on: headphones (A, register 2) or the
 * speaker (C, register 4); both muted is silence. */
static void screamer_update_volume(ScreamerState *s)
{
    uint16_t r1 = s->codec[1];
    int reg;

    if (!s->voice) {
        return;
    }
    if (!(r1 & R1_HEADPHONE_MUTE)) {
        reg = 2;
    } else if (!(r1 & R1_SPEAKER_MUTE)) {
        reg = 4;
    } else {
        AUD_set_volume_out(s->voice, 1, 0, 0);
        return;
    }
    AUD_set_volume_out(s->voice, 0,
                          screamer_atten(s->codec[reg] >> 6),
                          screamer_atten(s->codec[reg]));
}

static void screamer_out_cb(void *opaque, int avail);

/* (Re)open the output voice for the current rate and byte order */
static void screamer_open_voice(ScreamerState *s)
{
    struct audsettings as;
    int rate = screamer_rate(s);
    bool le = s->byteswap & 1;

    if (s->voice && rate == s->voice_rate && le == s->voice_le) {
        return;
    }
    as.freq = rate;
    as.nchannels = 2;
    as.fmt = AUDIO_FORMAT_S16;
    as.endianness = le ? 0 : 1;
    s->voice = AUD_open_out(&s->card, s->voice, "screamer.out", s,
                            screamer_out_cb, &as);
    s->voice_rate = rate;
    s->voice_le = le;
    screamer_update_volume(s);
}

/* the guest started an output command: play it */
static void screamer_dma_out(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;

    s->out_io = io;
    s->out_done = 0;
    screamer_open_voice(s);
    if (s->voice) {
        AUD_set_active_out(s->voice, true);
    }
}

static void screamer_finish(DBDMA_io *io)
{
    io->len = 0;
    io->dma_end(io);
}

/* The backend wants up to avail bytes: give it what the guest queued.
 * Completing a command can start the next one (screamer_dma_out again). */
static void screamer_out_cb(void *opaque, int avail)
{
    ScreamerState *s = opaque;
    uint8_t buf[4096];

    while (avail > 0) {
        DBDMA_io *io = s->out_io;
        int n, written;

        if (!io) {
            /* nothing queued: stop until the guest plays again */
            AUD_set_active_out(s->voice, false);
            return;
        }
        n = MIN(MIN(avail, (int)sizeof(buf)), io->len - (int)s->out_done);
        if (n > 0) {
            dma_memory_read(&address_space_memory, io->addr + s->out_done,
                            buf, n, MEMTXATTRS_UNSPECIFIED);
            written = AUD_write(s->voice, buf, n);
            s->out_done += written;
            s->frame_count += written / 4;
            avail -= written;
            if (written < n) {
                return;
            }
        }
        if (s->out_done >= io->len) {
            s->out_io = NULL;
            screamer_finish(io);
        }
    }
}

/* recording: silence, one command per its length in time */
static void screamer_dma_in(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;
    int64_t ns = (int64_t)io->len * NANOSECONDS_PER_SECOND /
                 (screamer_rate(s) * 4);

    s->in_io = io;
    timer_mod(s->in_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void screamer_in_timer(void *opaque)
{
    ScreamerState *s = opaque;
    DBDMA_io *io = s->in_io;
    uint8_t zero[4096] = { 0 };
    int done;

    if (!io) {
        return;
    }
    s->in_io = NULL;
    for (done = 0; done < io->len; done += sizeof(zero)) {
        dma_memory_write(&address_space_memory, io->addr + done, zero,
                         MIN(sizeof(zero), io->len - done),
                         MEMTXATTRS_UNSPECIFIED);
    }
    screamer_finish(io);
}

/*
 * DBDMA flush, also when the guest stops the channel: drop the command
 * being played, and say so, or the channel never runs again (DBDMA starts
 * no command while one is processing).
 */
static void screamer_dma_flush_out(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;

    if (s->out_io == io) {
        s->out_io = NULL;
    }
    io->processing = false;
}

static void screamer_dma_flush_in(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;

    if (s->in_io == io) {
        s->in_io = NULL;
        timer_del(s->in_timer);
    }
    io->processing = false;
}

static void screamer_codec_command(ScreamerState *s, uint32_t val)
{
    int addr = (val & CODEC_ADDR_MASK) >> CODEC_ADDR_SHIFT;

    if (addr == 7) {
        /* read back: register in bits 1-3 */
        s->readback = s->codec[(val >> 1) & 7];
        s->readback_valid = true;
    } else {
        s->codec[addr] = val & CODEC_DATA_MASK;
        s->readback_valid = false;
        if (addr == 1 || addr == 2 || addr == 4) {
            screamer_update_volume(s);
        }
    }
    trace_screamer_codec(addr, val & CODEC_DATA_MASK);
}

static uint64_t screamer_read(void *opaque, hwaddr addr, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t val = 0;

    switch (addr & ~0xf) {
    case REG_CONTROL:
        val = s->control;
        break;
    case REG_CODEC_CTRL:
        val = s->codec_ctrl & ~CODEC_BUSY;     /* commands finish at once */
        break;
    case REG_CODEC_STAT:
        val = STAT_VALID;
        if (s->readback_valid) {
            val |= (s->readback & CODEC_DATA_MASK) << 4;
        } else {
            val |= (3 << STAT_REVISION_SHIFT) | (1 << STAT_MAKER_SHIFT);
        }
        break;
    case REG_CLIP_COUNT:
        val = s->clip_count;
        s->clip_count = 0;
        break;
    case REG_BYTESWAP:
        val = s->byteswap;
        break;
    case REG_FRAME_COUNT:
        val = s->frame_count;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "screamer: read 0x%" HWADDR_PRIx "\n",
                      addr);
    }
    trace_screamer_read(addr, val);
    return val;
}

static void screamer_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    ScreamerState *s = opaque;

    trace_screamer_write(addr, val);
    switch (addr & ~0xf) {
    case REG_CONTROL:
        s->control = val;
        if (s->voice && screamer_rate(s) != s->voice_rate) {
            screamer_open_voice(s);
        }
        break;
    case REG_CODEC_CTRL:
        s->codec_ctrl = val;
        screamer_codec_command(s, val);
        break;
    case REG_BYTESWAP:
        s->byteswap = val;
        break;
    case REG_FRAME_COUNT:
        s->frame_count = val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "screamer: write 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n",
                      addr, val);
    }
}

static const MemoryRegionOps screamer_ops = {
    .read = screamer_read,
    .write = screamer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* DBDMA's interrupts, passed on to the machine's */
static void screamer_dma_irq(void *opaque, int n, int level)
{
    ScreamerState *s = opaque;

    qemu_set_irq(s->irq[1 + n], level);
}

static void screamer_reset(DeviceState *dev)
{
    ScreamerState *s = SCREAMER(dev);

    s->control = 0;
    s->codec_ctrl = 0;
    s->readback_valid = false;
    s->clip_count = 0;
    s->byteswap = 0;
    s->frame_count = 0;
    memset(s->codec, 0, sizeof(s->codec));
    s->out_io = NULL;
    s->in_io = NULL;
    timer_del(s->in_timer);
    if (s->voice) {
        AUD_set_active_out(s->voice, false);
    }
}

static void screamer_realize(DeviceState *dev, Error **errp)
{
    ScreamerState *s = SCREAMER(dev);

    if (!s->dbdma) {
        error_setg(errp, "screamer: no dbdma link");
        return;
    }
    if (!AUD_register_card("screamer", &s->card, errp)) {
        return;
    }
    s->dma_irq[0] = qemu_allocate_irq(screamer_dma_irq, s, 0);
    s->dma_irq[1] = qemu_allocate_irq(screamer_dma_irq, s, 1);
    DBDMA_register_channel(s->dbdma, SCREAMER_DMA_OUT, s->dma_irq[0],
                           screamer_dma_out, screamer_dma_flush_out, s);
    DBDMA_register_channel(s->dbdma, SCREAMER_DMA_IN, s->dma_irq[1],
                           screamer_dma_in, screamer_dma_flush_in, s);
    s->in_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, screamer_in_timer, s);
}

static void screamer_unrealize(DeviceState *dev)
{
    ScreamerState *s = SCREAMER(dev);

    timer_free(s->in_timer);
    if (s->voice) {
        AUD_close_out(&s->card, s->voice);
    }
    AUD_remove_card(&s->card);
}

static void screamer_init(Object *obj)
{
    ScreamerState *s = SCREAMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->mem, obj, &screamer_ops, s, "screamer", 0x1000);
    sysbus_init_mmio(sbd, &s->mem);
    for (i = 0; i < 3; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    object_property_add_link(obj, "dbdma", TYPE_MAC_DBDMA,
                             (Object **)&s->dbdma,
                             qdev_prop_allow_set_link_before_realize, 0);
}

static const Property screamer_properties[] = {
    DEFINE_AUDIO_PROPERTIES(ScreamerState, card),
};

static const VMStateDescription vmstate_screamer = {
    .name = "screamer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, ScreamerState),
        VMSTATE_UINT32(codec_ctrl, ScreamerState),
        VMSTATE_UINT32(readback, ScreamerState),
        VMSTATE_BOOL(readback_valid, ScreamerState),
        VMSTATE_UINT32(clip_count, ScreamerState),
        VMSTATE_UINT32(byteswap, ScreamerState),
        VMSTATE_UINT32(frame_count, ScreamerState),
        VMSTATE_UINT16_ARRAY(codec, ScreamerState, 8),
        VMSTATE_END_OF_LIST()
    },
};

static void screamer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = screamer_realize;
    dc->unrealize = screamer_unrealize;
    device_class_set_legacy_reset(dc, screamer_reset);
    dc->vmsd = &vmstate_screamer;
    device_class_set_props(dc, screamer_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Screamer sound chip (Power Mac davbus)";
    dc->user_creatable = false;
}

static const TypeInfo screamer_info = {
    .name = TYPE_SCREAMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ScreamerState),
    .instance_init = screamer_init,
    .class_init = screamer_class_init,
};

static void screamer_register_types(void)
{
    type_register_static(&screamer_info);
}

type_init(screamer_register_types)
