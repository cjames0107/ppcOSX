/*
 * Screamer: the Power Mac's sound chip (an AWACS-compatible codec on the
 * mac-io's davbus, fed by DBDMA).
 */
#ifndef HW_AUDIO_SCREAMER_H
#define HW_AUDIO_SCREAMER_H

#include "hw/sysbus.h"
#include "audio/audio.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_SCREAMER "screamer"
OBJECT_DECLARE_SIMPLE_TYPE(ScreamerState, SCREAMER)

/* where the davbus is in the mac-io, and its DBDMA channels */
#define SCREAMER_MACIO_OFFSET   0x14000
#define SCREAMER_DMA_OUT        0x10        /* mac-io 0x8800 */
#define SCREAMER_DMA_IN         0x12        /* mac-io 0x8900 */

struct ScreamerState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    void *dbdma;
    /* out: 0 the codec (port change), 1 output DMA, 2 input DMA */
    qemu_irq irq[3];
    qemu_irq dma_irq[2];

    QEMUSoundCard card;
    SWVoiceOut *voice;
    int voice_rate;
    bool voice_le;

    uint32_t control;
    uint32_t codec_ctrl;
    uint32_t readback;          /* register value read back (command 7) */
    bool readback_valid;
    uint32_t clip_count;
    uint32_t byteswap;
    uint32_t frame_count;
    uint16_t codec[8];

    /* the DBDMA command being played, and how far */
    void *out_io;
    uint32_t out_done;
    /* recording is silence, delivered at the sample rate */
    void *in_io;
    QEMUTimer *in_timer;
};

#endif
