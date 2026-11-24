/*
 * PPCOSXFramebuffer: Mac OS X (PowerPC) display driver for the ppcosx-gpu
 * paravirtual graphics device.
 *
 * A 32-bit framebuffer, plus a user client that lets applications send GL
 * command batches to the host renderer. The device's registers and command
 * batches are big-endian, which is native here, so there is no byte
 * swapping anywhere in this driver.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>

#include "PPCOSXFramebuffer.h"
#include "ppcosx-gpu.h"
#include "ppcosx-gl-image.h"

#define super IOFramebuffer
OSDefineMetaClassAndStructors(PPCOSXFramebuffer, IOFramebuffer)

#define LOG(fmt, args...) IOLog("PPCOSXFramebuffer: " fmt "\n", ## args)

struct PPCOSXMode {
    UInt32 width;
    UInt32 height;
};

/* Display mode IDs are 1-based indexes into this table. */
static const PPCOSXMode gModes[] = {
    {  640,  480 },
    {  800,  600 },
    { 1024,  768 },
    { 1152,  870 },
    { 1280,  800 },
    { 1280, 1024 },
    { 1440,  900 },
    { 1600, 1200 },
    { 1920, 1080 },
    { 1920, 1200 },
};
#define kModeCount      (sizeof(gModes) / sizeof(gModes[0]))
#define kDefaultMode    3       /* 1024x768 */
#define kBatchBytes     4096

/* VRAM below this is the scanout's; GL readback areas live above it. */
#define kScanoutReserve (16 * 1024 * 1024)

static const PPCOSXMode * modeForID(IODisplayModeID id)
{
    if (id < 1 || id > (IODisplayModeID) kModeCount)
        return 0;
    return &gModes[id - 1];
}

UInt32 PPCOSXFramebuffer::readReg(UInt32 offset)
{
    return fRegs[offset / 4];
}

void PPCOSXFramebuffer::writeReg(UInt32 offset, UInt32 value)
{
    fRegs[offset / 4] = value;
    OSSynchronizeIO();
}

/* Run the first `bytes` of fBatch. Caller holds fBatchLock. */
IOReturn PPCOSXFramebuffer::submitBatch(UInt32 bytes)
{
    return runBatch(fBatchPhys, bytes);
}

/* Run a batch at any physically contiguous address. Caller holds the lock. */
IOReturn PPCOSXFramebuffer::runBatch(IOPhysicalAddress phys, UInt32 bytes)
{
    UInt32 status;

    writeReg(PPCOSX_GPU_REG_CMD_ADDR_LO, (UInt32) phys);
    writeReg(PPCOSX_GPU_REG_CMD_ADDR_HI, 0);
    writeReg(PPCOSX_GPU_REG_CMD_SIZE, bytes);
    writeReg(PPCOSX_GPU_REG_CMD_KICK, 1);   /* synchronous on the host */

    status = readReg(PPCOSX_GPU_REG_STATUS);
    writeReg(PPCOSX_GPU_REG_IRQ_STATUS, PPCOSX_GPU_IRQ_BATCH_DONE);
    if (status != PPCOSX_GPU_ST_OK) {
        LOG("batch failed, device status %lu", (unsigned long) status);
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

bool PPCOSXFramebuffer::glAvailable(void)
{
    return (readReg(PPCOSX_GPU_REG_FEATURES) & PPCOSX_GPU_F_GL) != 0;
}

/*
 * First-fit allocation of a page-rounded readback area for `ctx`, replacing
 * any it already has. Caller holds fBatchLock.
 */
bool PPCOSXFramebuffer::glAllocArea(UInt32 ctx, UInt32 bytes)
{
    UInt32 start = kScanoutReserve;
    UInt32 i;
    bool moved;

    bytes = (bytes + page_size - 1) & ~(page_size - 1);
    fGLArea[ctx].size = 0;

    do {
        moved = false;
        for (i = 1; i <= PPCOSX_GPU_GL_MAX_CONTEXTS; i++) {
            if (fGLArea[i].size && start < fGLArea[i].offset + fGLArea[i].size
                && fGLArea[i].offset < start + bytes) {
                start = fGLArea[i].offset + fGLArea[i].size;
                moved = true;
            }
        }
    } while (moved);

    if (start > fVRAMSize || bytes > fVRAMSize - start)
        return false;
    fGLArea[ctx].offset = start;
    fGLArea[ctx].size = bytes;
    return true;
}

/* CTX_CREATE or CTX_RESIZE. Caller holds fBatchLock. */
IOReturn PPCOSXFramebuffer::glContextCommand(UInt32 opcode, UInt32 ctx,
                                             UInt32 width, UInt32 height,
                                             UInt32 format)
{
    struct ppcosx_gpu_cmd_gl_ctx_create * cmd;

    cmd = (struct ppcosx_gpu_cmd_gl_ctx_create *) fBatch;
    cmd->hdr.opcode = opcode;
    cmd->hdr.length = sizeof(*cmd);
    cmd->ctx = ctx;
    cmd->width = width;
    cmd->height = height;
    cmd->format = format;
    return submitBatch(sizeof(*cmd));
}

IOReturn PPCOSXFramebuffer::glCreateContext(UInt32 width, UInt32 height,
                                            UInt32 format, UInt32 * ctx)
{
    IOReturn ret = kIOReturnNoResources;
    UInt32 id;

    if (width < 1 || width > PPCOSX_GPU_GL_MAX_SIZE ||
        height < 1 || height > PPCOSX_GPU_GL_MAX_SIZE)
        return kIOReturnBadArgument;

    IOLockLock(fBatchLock);
    for (id = 1; id <= PPCOSX_GPU_GL_MAX_CONTEXTS; id++)
        if (!(fGLContexts & (1 << id)))
            break;
    if (id <= PPCOSX_GPU_GL_MAX_CONTEXTS &&
        glAllocArea(id, width * height * 4)) {
        ret = glContextCommand(PPCOSX_GPU_CMD_GL_CTX_CREATE, id, width,
                               height, format);
        if (ret == kIOReturnSuccess) {
            fGLContexts |= 1 << id;
            *ctx = id;
        } else {
            fGLArea[id].size = 0;
        }
    }
    IOLockUnlock(fBatchLock);
    return ret;
}

IOReturn PPCOSXFramebuffer::glResizeContext(UInt32 ctx, UInt32 width,
                                            UInt32 height, UInt32 format)
{
    IOReturn ret = kIOReturnNoResources;
    UInt32 oldOffset, oldSize;

    if (width < 1 || width > PPCOSX_GPU_GL_MAX_SIZE ||
        height < 1 || height > PPCOSX_GPU_GL_MAX_SIZE ||
        ctx < 1 || ctx > PPCOSX_GPU_GL_MAX_CONTEXTS)
        return kIOReturnBadArgument;

    IOLockLock(fBatchLock);
    if (fGLContexts & (1 << ctx)) {
        oldOffset = fGLArea[ctx].offset;
        oldSize = fGLArea[ctx].size;
        if (glAllocArea(ctx, width * height * 4))
            ret = glContextCommand(PPCOSX_GPU_CMD_GL_CTX_RESIZE, ctx, width,
                                   height, format);
        if (ret != kIOReturnSuccess) {
            fGLArea[ctx].offset = oldOffset;
            fGLArea[ctx].size = oldSize;
        }
    }
    IOLockUnlock(fBatchLock);
    return ret;
}

IODeviceMemory * PPCOSXFramebuffer::glReadbackMemory(UInt32 ctx,
                                                     UInt32 * offset)
{
    IODeviceMemory * mem = 0;

    IOLockLock(fBatchLock);
    if (ctx >= 1 && ctx <= PPCOSX_GPU_GL_MAX_CONTEXTS && fGLArea[ctx].size) {
        if (offset)
            *offset = fGLArea[ctx].offset;
        mem = IODeviceMemory::withSubRange(fVRAM, fGLArea[ctx].offset,
                                           fGLArea[ctx].size);
    }
    IOLockUnlock(fBatchLock);
    return mem;
}

void PPCOSXFramebuffer::glDestroyContext(UInt32 ctx)
{
    struct ppcosx_gpu_cmd_gl_ctx * cmd;

    IOLockLock(fBatchLock);
    if (ctx >= 1 && ctx <= PPCOSX_GPU_GL_MAX_CONTEXTS &&
        (fGLContexts & (1 << ctx))) {
        cmd = (struct ppcosx_gpu_cmd_gl_ctx *) fBatch;
        cmd->hdr.opcode = PPCOSX_GPU_CMD_GL_CTX_DESTROY;
        cmd->hdr.length = sizeof(*cmd);
        cmd->ctx = ctx;
        submitBatch(sizeof(*cmd));
        fGLContexts &= ~(1 << ctx);
        fGLArea[ctx].size = 0;
    }
    IOLockUnlock(fBatchLock);
}

IOReturn PPCOSXFramebuffer::glSetOverlay(UInt32 ctx, SInt32 x, SInt32 y,
                                         UInt32 width, UInt32 height,
                                         bool visible)
{
    struct ppcosx_gpu_cmd_gl_overlay_set * cmd;
    IOReturn ret = kIOReturnNotOpen;

    if (width > PPCOSX_GPU_GL_MAX_SIZE || height > PPCOSX_GPU_GL_MAX_SIZE ||
        ctx < 1 || ctx > PPCOSX_GPU_GL_MAX_CONTEXTS)
        return kIOReturnBadArgument;

    IOLockLock(fBatchLock);
    if (fGLContexts & (1 << ctx)) {
        cmd = (struct ppcosx_gpu_cmd_gl_overlay_set *) fBatch;
        cmd->hdr.opcode = PPCOSX_GPU_CMD_GL_OVERLAY_SET;
        cmd->hdr.length = sizeof(*cmd);
        cmd->ctx = ctx;
        cmd->x = x;
        cmd->y = y;
        cmd->width = width;
        cmd->height = height;
        cmd->flags = visible ? PPCOSX_GPU_OVERLAY_VISIBLE : 0;
        ret = submitBatch(sizeof(*cmd));
    }
    IOLockUnlock(fBatchLock);
    return ret;
}

IOReturn PPCOSXFramebuffer::glSubmit(IOPhysicalAddress batch, UInt32 bytes)
{
    IOReturn ret;

    IOLockLock(fBatchLock);
    ret = runBatch(batch, bytes);
    IOLockUnlock(fBatchLock);
    return ret;
}

IOReturn PPCOSXFramebuffer::newUserClient(task_t owningTask,
    void * securityID, UInt32 type, IOUserClient ** handler)
{
    PPCOSXGLUserClient * client;

    if (type != kPPCOSXGLConnectType)
        return super::newUserClient(owningTask, securityID, type, handler);

    if (!glAvailable())
        return kIOReturnUnsupported;

    client = new PPCOSXGLUserClient;
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) ||
        !client->attach(this)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnNoMemory;
    }
    *handler = client;
    return kIOReturnSuccess;
}

IOReturn PPCOSXFramebuffer::programScanout(IODisplayModeID id)
{
    const PPCOSXMode * mode = modeForID(id);
    struct ppcosx_gpu_cmd_set_scanout * cmd;
    IOReturn ret;

    if (!mode || mode->width * mode->height * 4 > fVRAMSize ||
        mode->width * mode->height * 4 > kScanoutReserve)
        return kIOReturnBadArgument;

    IOLockLock(fBatchLock);
    cmd = (struct ppcosx_gpu_cmd_set_scanout *) fBatch;
    cmd->hdr.opcode = PPCOSX_GPU_CMD_SET_SCANOUT;
    cmd->hdr.length = sizeof(*cmd);
    cmd->width = mode->width;
    cmd->height = mode->height;
    cmd->stride = mode->width * 4;
    cmd->offset = 0;
    ret = submitBatch(sizeof(*cmd));
    IOLockUnlock(fBatchLock);

    if (ret == kIOReturnSuccess)
        fMode = id;
    return ret;
}

bool PPCOSXFramebuffer::start(IOService * provider)
{
    IODeviceMemory * vram;
    UInt32 magic, features;

    fDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fDevice)
        return false;

    /* The command channel reads guest memory, so it needs bus mastering. */
    fDevice->setMemoryEnable(true);
    fDevice->setBusMasterEnable(true);

    fRegMap = fDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!fRegMap) {
        LOG("cannot map registers");
        return false;
    }
    fRegs = (volatile UInt32 *) fRegMap->getVirtualAddress();

    magic = readReg(PPCOSX_GPU_REG_MAGIC);
    features = readReg(PPCOSX_GPU_REG_FEATURES);
    if (magic != PPCOSX_GPU_MAGIC || !(features & PPCOSX_GPU_F_SCANOUT)) {
        LOG("unexpected device (magic %08lx, features %08lx)",
            (unsigned long) magic, (unsigned long) features);
        return false;
    }

    vram = fDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!vram) {
        LOG("no scanout memory");
        return false;
    }
    fVRAM = vram;
    fVRAM->retain();
    fVRAMSize = readReg(PPCOSX_GPU_REG_VRAM_SIZE);
    if (fVRAMSize > fVRAM->getLength())
        fVRAMSize = fVRAM->getLength();

    fBatchLock = IOLockAlloc();
    fBatch = (UInt32 *) IOMallocContiguous(kBatchBytes, kBatchBytes,
                                           &fBatchPhys);
    if (!fBatchLock || !fBatch) {
        LOG("cannot allocate command buffer");
        return false;
    }

    fMode = kDefaultMode;

    LOG("version %lx, %lu MB scanout memory, host GL %s",
        (unsigned long) readReg(PPCOSX_GPU_REG_VERSION),
        (unsigned long) (fVRAMSize >> 20),
        glAvailable() ? "available" : "unavailable");

    return super::start(provider);
}

void PPCOSXFramebuffer::stop(IOService * provider)
{
    super::stop(provider);
}

void PPCOSXFramebuffer::free()
{
    if (fBatch) {
        IOFreeContiguous(fBatch, kBatchBytes);
        fBatch = 0;
    }
    if (fBatchLock) {
        IOLockFree(fBatchLock);
        fBatchLock = 0;
    }
    if (fVRAM) {
        fVRAM->release();
        fVRAM = 0;
    }
    if (fRegMap) {
        fRegMap->release();
        fRegMap = 0;
    }
    super::free();
}

IOReturn PPCOSXFramebuffer::enableController(void)
{
    return programScanout(fMode);
}

IODeviceMemory * PPCOSXFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    const PPCOSXMode * mode = modeForID(fMode);

    if (aperture != kIOFBSystemAperture || !mode)
        return 0;
    return IODeviceMemory::withSubRange(fVRAM, 0,
                                        mode->width * mode->height * 4);
}

IODeviceMemory * PPCOSXFramebuffer::getVRAMRange(void)
{
    fVRAM->retain();
    return fVRAM;
}

const char * PPCOSXFramebuffer::getPixelFormats(void)
{
    static const char formats[] = IO32BitDirectPixels "\0";
    return formats;
}

IOItemCount PPCOSXFramebuffer::getDisplayModeCount(void)
{
    IOItemCount count = 0;

    for (UInt32 i = 0; i < kModeCount; i++)
        if (gModes[i].width * gModes[i].height * 4 <= fVRAMSize)
            count++;
    return count;
}

IOReturn PPCOSXFramebuffer::getDisplayModes(IODisplayModeID * allDisplayModes)
{
    for (UInt32 i = 0; i < kModeCount; i++)
        if (gModes[i].width * gModes[i].height * 4 <= fVRAMSize)
            *allDisplayModes++ = i + 1;
    return kIOReturnSuccess;
}

IOReturn PPCOSXFramebuffer::getInformationForDisplayMode(
    IODisplayModeID displayMode, IODisplayModeInformation * info)
{
    const PPCOSXMode * mode = modeForID(displayMode);

    if (!mode || !info)
        return kIOReturnBadArgument;

    bzero(info, sizeof(*info));
    info->nominalWidth = mode->width;
    info->nominalHeight = mode->height;
    info->refreshRate = 60 << 16;
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    if (displayMode == kDefaultMode)
        info->flags |= kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

UInt64 PPCOSXFramebuffer::getPixelFormatsForDisplayMode(
    IODisplayModeID displayMode, IOIndex depth)
{
    return 0;
}

IOReturn PPCOSXFramebuffer::getPixelInformation(IODisplayModeID displayMode,
    IOIndex depth, IOPixelAperture aperture, IOPixelInformation * pixelInfo)
{
    const PPCOSXMode * mode = modeForID(displayMode);

    if (!mode || !pixelInfo || depth != 0 || aperture != kIOFBSystemAperture)
        return kIOReturnBadArgument;

    bzero(pixelInfo, sizeof(*pixelInfo));
    pixelInfo->bytesPerRow = mode->width * 4;
    pixelInfo->bytesPerPlane = 0;
    pixelInfo->bitsPerPixel = 32;
    pixelInfo->pixelType = kIORGBDirectPixels;
    pixelInfo->componentCount = 3;
    pixelInfo->bitsPerComponent = 8;
    pixelInfo->componentMasks[0] = 0x00ff0000;
    pixelInfo->componentMasks[1] = 0x0000ff00;
    pixelInfo->componentMasks[2] = 0x000000ff;
    strcpy(pixelInfo->pixelFormat, IO32BitDirectPixels);
    pixelInfo->activeWidth = mode->width;
    pixelInfo->activeHeight = mode->height;
    return kIOReturnSuccess;
}

IOReturn PPCOSXFramebuffer::getCurrentDisplayMode(
    IODisplayModeID * displayMode, IOIndex * depth)
{
    if (displayMode)
        *displayMode = fMode;
    if (depth)
        *depth = 0;
    return kIOReturnSuccess;
}

IOReturn PPCOSXFramebuffer::setDisplayMode(IODisplayModeID displayMode,
                                           IOIndex depth)
{
    if (depth != 0)
        return kIOReturnBadArgument;
    return programScanout(displayMode);
}

IOReturn PPCOSXFramebuffer::getStartupDisplayMode(
    IODisplayModeID * displayMode, IOIndex * depth)
{
    if (displayMode)
        *displayMode = kDefaultMode;
    if (depth)
        *depth = 0;
    return kIOReturnSuccess;
}

/* 32-bit direct colour only: there is no CLUT or gamma hardware to load. */
IOReturn PPCOSXFramebuffer::setCLUTWithEntries(IOColorEntry * colors,
    UInt32 index, UInt32 numEntries, IOOptionBits options)
{
    return kIOReturnSuccess;
}

IOReturn PPCOSXFramebuffer::setGammaTable(UInt32 channelCount,
    UInt32 dataCount, UInt32 dataWidth, void * data)
{
    return kIOReturnSuccess;
}

/* ---- GL user client ---------------------------------------------------- */

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(PPCOSXGLUserClient, IOUserClient)

/* Room for the context bind the driver puts in front of client commands. */
#define kBindBytes  sizeof(struct ppcosx_gpu_cmd_gl_ctx)

bool PPCOSXGLUserClient::initWithTask(task_t owningTask, void * securityID,
                                      UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    fTask = owningTask;
    return true;
}

bool PPCOSXGLUserClient::start(IOService * provider)
{
    fOwner = OSDynamicCast(PPCOSXFramebuffer, provider);
    if (!fOwner || !super::start(provider))
        return false;

    fShared = IOBufferMemoryDescriptor::withOptions(
                  kIODirectionOutIn | kIOMemoryKernelUserShared,
                  kPPCOSXGLBufferBytes, page_size);
    fBatch = (UInt32 *) IOMallocContiguous(kPPCOSXGLBufferBytes + page_size,
                                           page_size, &fBatchPhys);
    if (!fShared || !fBatch)
        return false;
    bzero(fShared->getBytesNoCopy(), kPPCOSXGLBufferBytes);
    return true;
}

void PPCOSXGLUserClient::free()
{
    if (fBatch) {
        IOFreeContiguous(fBatch, kPPCOSXGLBufferBytes + page_size);
        fBatch = 0;
    }
    if (fShared) {
        fShared->release();
        fShared = 0;
    }
    super::free();
}

IOReturn PPCOSXGLUserClient::clientClose(void)
{
    if (fContext && fOwner) {
        fOwner->glDestroyContext(fContext);
        fContext = 0;
    }
    if (!isInactive())
        terminate();
    return kIOReturnSuccess;
}

IOReturn PPCOSXGLUserClient::clientDied(void)
{
    return clientClose();
}

IOReturn PPCOSXGLUserClient::clientMemoryForType(UInt32 type,
    IOOptionBits * options, IOMemoryDescriptor ** memory)
{
    if (type == 0 && fShared) {
        /* Command buffer. */
        fShared->retain();
        *memory = fShared;
        *options = 0;
        return kIOReturnSuccess;
    }
    if (type == 1 && fContext) {
        /* This context's readback area, as it is now; remap after resize. */
        *memory = fOwner->glReadbackMemory(fContext, 0);
        *options = kIOMapReadOnly;
        return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    return kIOReturnBadArgument;
}

IOReturn PPCOSXGLUserClient::resizeContext(void * width, void * height,
                                           void * format)
{
    if (!fContext)
        return kIOReturnNotOpen;
    return fOwner->glResizeContext(fContext, (UInt32) width, (UInt32) height,
                                   (UInt32) format);
}

/* Returns the context's id, which the application needs for its overlay
 * key colour. */
IOReturn PPCOSXGLUserClient::createContext(void * width, void * height,
                                           void * format, void * ctxOut)
{
    IOReturn ret;

    if (fContext)
        return kIOReturnExclusiveAccess;
    ret = fOwner->glCreateContext((UInt32) width, (UInt32) height,
                                  (UInt32) format, &fContext);
    if (ret == kIOReturnSuccess)
        *(UInt32 *) ctxOut = fContext;
    return ret;
}

/* Place (or hide) this connection's own overlay, at screen pixels. */
IOReturn PPCOSXGLUserClient::setOverlay(void * x, void * y, void * width,
                                        void * height, void * visible)
{
    if (!fContext)
        return kIOReturnNotOpen;
    return fOwner->glSetOverlay(fContext, (SInt32) x, (SInt32) y,
                                (UInt32) width, (UInt32) height,
                                visible != 0);
}

/*
 * Run the first `bytes` of the shared buffer against this client's context.
 * The commands are copied out first, so the client cannot change them
 * between the check below and the device reading them.
 */
IOReturn PPCOSXGLUserClient::submit(void * bytesArg)
{
    UInt32 bytes = (UInt32) bytesArg;
    struct ppcosx_gpu_cmd_gl_ctx * bind;
    IODeviceMemory * area;
    UInt8 * cmds;
    UInt32 pos = 0;
    UInt32 readbackOffset = 0, readbackSize = 0;

    if (!fContext)
        return kIOReturnNotOpen;
    if (bytes == 0)
        return kIOReturnSuccess;
    if (bytes > kPPCOSXGLBufferBytes || (bytes & 3))
        return kIOReturnBadArgument;

    bind = (struct ppcosx_gpu_cmd_gl_ctx *) fBatch;
    bind->hdr.opcode = PPCOSX_GPU_CMD_GL_CTX_BIND;
    bind->hdr.length = sizeof(*bind);
    bind->ctx = fContext;

    cmds = (UInt8 *) fBatch + kBindBytes;
    bcopy(fShared->getBytesNoCopy(), cmds, bytes);

    while (pos < bytes) {
        struct ppcosx_gpu_cmd_hdr * hdr;

        if (bytes - pos < sizeof(*hdr))
            return kIOReturnBadArgument;
        hdr = (struct ppcosx_gpu_cmd_hdr *) (cmds + pos);
        if (hdr->length < sizeof(*hdr) || (hdr->length & 3) ||
            hdr->length > bytes - pos)
            return kIOReturnBadArgument;
        /* Applications may draw, present, read back and hand frames to
         * their own overlay; nothing else. */
        if (hdr->opcode == PPCOSX_GPU_CMD_GL_READBACK ||
            hdr->opcode == PPCOSX_GPU_CMD_GL_READPIXELS) {
            /* Pixels only ever go to this context's own area. */
            if (!readbackOffset) {
                area = fOwner->glReadbackMemory(fContext, &readbackOffset);
                if (!area)
                    return kIOReturnNoMemory;
                readbackSize = area->getLength();
                area->release();
            }
            if (hdr->opcode == PPCOSX_GPU_CMD_GL_READBACK) {
                if (hdr->length != sizeof(struct ppcosx_gpu_cmd_gl_readback))
                    return kIOReturnBadArgument;
                ((struct ppcosx_gpu_cmd_gl_readback *) hdr)->offset =
                    readbackOffset;
            } else {
                struct ppcosx_gpu_cmd_gl_readpixels * rp =
                    (struct ppcosx_gpu_cmd_gl_readpixels *) hdr;
                UInt64 bytes;

                if (hdr->length != sizeof(*rp) ||
                    rp->width > PPCOSX_GPU_GL_MAX_SIZE ||
                    rp->height > PPCOSX_GPU_GL_MAX_SIZE)
                    return kIOReturnBadArgument;
                bytes = (UInt64) rp->width * rp->height *
                        ppcosx_gl_pixel_bytes(rp->format, rp->type);
                if (bytes > readbackSize)
                    return kIOReturnBadArgument;
                rp->offset = readbackOffset;
            }
        } else if (hdr->opcode == PPCOSX_GPU_CMD_GL_OVERLAY_FRAME) {
            if (hdr->length != sizeof(struct ppcosx_gpu_cmd_hdr))
                return kIOReturnBadArgument;
        } else if (hdr->opcode != PPCOSX_GPU_CMD_GL_PRESENT &&
                   (hdr->opcode < PPCOSX_GPU_CMD_GL_FIRST ||
                    hdr->opcode > PPCOSX_GPU_CMD_GL_LAST)) {
            return kIOReturnNotPrivileged;
        }
        pos += hdr->length;
    }

    return fOwner->glSubmit(fBatchPhys, kBindBytes + bytes);
}

IOExternalMethod * PPCOSXGLUserClient::getTargetAndMethodForIndex(
    IOService ** target, UInt32 index)
{
    static const IOExternalMethod methods[] = {
        /* 0: createContext(width, height, format) -> ctx */
        { 0, (IOMethod) &PPCOSXGLUserClient::createContext,
          kIOUCScalarIScalarO, 3, 1 },
        /* 1: submit(bytes) */
        { 0, (IOMethod) &PPCOSXGLUserClient::submit,
          kIOUCScalarIScalarO, 1, 0 },
        /* 2: resizeContext(width, height, format) */
        { 0, (IOMethod) &PPCOSXGLUserClient::resizeContext,
          kIOUCScalarIScalarO, 3, 0 },
        /* 3: setOverlay(x, y, width, height, visible) */
        { 0, (IOMethod) &PPCOSXGLUserClient::setOverlay,
          kIOUCScalarIScalarO, 5, 0 },
    };

    if (index >= sizeof(methods) / sizeof(methods[0]))
        return 0;
    *target = this;
    return (IOExternalMethod *) &methods[index];
}
