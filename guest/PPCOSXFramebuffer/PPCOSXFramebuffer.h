/*
 * PPCOSXFramebuffer: Mac OS X (PowerPC) display driver for the ppcosx-gpu
 * paravirtual graphics device.
 *
 * Licensed under the GNU GPL, version 2 or later.
 */

#ifndef PPCOSX_FRAMEBUFFER_H
#define PPCOSX_FRAMEBUFFER_H

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

/* IOServiceOpen() type that selects the GL command channel. */
#define kPPCOSXGLConnectType    0x7078676c      /* 'pxgl' */

/* Size of the buffer a GL client shares with the driver. */
#define kPPCOSXGLBufferBytes    (256 * 1024)

class PPCOSXFramebuffer : public IOFramebuffer
{
    OSDeclareDefaultStructors(PPCOSXFramebuffer)

private:
    IOPCIDevice *       fDevice;
    IOMemoryMap *       fRegMap;
    volatile UInt32 *   fRegs;
    IODeviceMemory *    fVRAM;
    UInt32              fVRAMSize;

    UInt32 *            fBatch;         /* one wired, contiguous page */
    IOPhysicalAddress   fBatchPhys;
    IOLock *            fBatchLock;

    IODisplayModeID     fMode;          /* 1-based index into the mode table */
    UInt32              fGLContexts;    /* bit n set: context id n in use */

    /* Readback areas in VRAM above the scanout, indexed by context id. */
    struct { UInt32 offset; UInt32 size; } fGLArea[17];

    bool     glAllocArea(UInt32 ctx, UInt32 bytes);
    IOReturn glContextCommand(UInt32 opcode, UInt32 ctx, UInt32 width,
                              UInt32 height, UInt32 format);

    UInt32   readReg(UInt32 offset);
    void     writeReg(UInt32 offset, UInt32 value);
    IOReturn submitBatch(UInt32 bytes);
    IOReturn runBatch(IOPhysicalAddress phys, UInt32 bytes);
    IOReturn programScanout(IODisplayModeID mode);

public:
    /* GL command channel, used by PPCOSXGLUserClient. */
    bool     glAvailable(void);
    IOReturn glCreateContext(UInt32 width, UInt32 height, UInt32 format,
                             UInt32 * ctx);
    IOReturn glResizeContext(UInt32 ctx, UInt32 width, UInt32 height,
                             UInt32 format);
    void     glDestroyContext(UInt32 ctx);
    IOReturn glSetOverlay(UInt32 ctx, SInt32 x, SInt32 y, UInt32 width,
                          UInt32 height, bool visible);
    /* Where READBACK puts this context's frames; retained by the caller. */
    IODeviceMemory * glReadbackMemory(UInt32 ctx, UInt32 * offset);
    IOReturn glSubmit(IOPhysicalAddress batch, UInt32 bytes);

    virtual IOReturn newUserClient(task_t owningTask, void * securityID,
                        UInt32 type, IOUserClient ** handler);

    virtual bool start(IOService * provider);
    virtual void stop(IOService * provider);
    virtual void free();

    virtual IOReturn enableController(void);
    virtual IODeviceMemory * getApertureRange(IOPixelAperture aperture);
    virtual IODeviceMemory * getVRAMRange(void);
    virtual const char * getPixelFormats(void);
    virtual IOItemCount getDisplayModeCount(void);
    virtual IOReturn getDisplayModes(IODisplayModeID * allDisplayModes);
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                        IODisplayModeInformation * info);
    virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                        IOIndex depth);
    virtual IOReturn getPixelInformation(IODisplayModeID displayMode,
                        IOIndex depth, IOPixelAperture aperture,
                        IOPixelInformation * pixelInfo);
    virtual IOReturn getCurrentDisplayMode(IODisplayModeID * displayMode,
                        IOIndex * depth);
    virtual IOReturn setDisplayMode(IODisplayModeID displayMode,
                        IOIndex depth);
    virtual IOReturn getStartupDisplayMode(IODisplayModeID * displayMode,
                        IOIndex * depth);
    virtual IOReturn setCLUTWithEntries(IOColorEntry * colors, UInt32 index,
                        UInt32 numEntries, IOOptionBits options);
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                        UInt32 dataWidth, void * data);
};

/*
 * One per application GL connection. The application fills a shared buffer
 * with GL commands and asks for them to be run; the driver copies them to a
 * private buffer, checks that they are all unprivileged, and prefixes the
 * bind of this connection's own context.
 */
class PPCOSXGLUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(PPCOSXGLUserClient)

private:
    PPCOSXFramebuffer *         fOwner;
    task_t                      fTask;
    IOBufferMemoryDescriptor *  fShared;        /* mapped into the client */
    UInt32 *                    fBatch;         /* private, contiguous */
    IOPhysicalAddress           fBatchPhys;
    UInt32                      fContext;       /* 0: none yet */

    IOReturn createContext(void * width, void * height, void * format,
                           void * ctxOut);
    IOReturn resizeContext(void * width, void * height, void * format);
    IOReturn setOverlay(void * x, void * y, void * width, void * height,
                        void * visible);
    IOReturn submit(void * bytes);

public:
    virtual bool initWithTask(task_t owningTask, void * securityID,
                              UInt32 type);
    virtual bool start(IOService * provider);
    virtual void free();
    virtual IOReturn clientClose(void);
    virtual IOReturn clientDied(void);
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options,
                                         IOMemoryDescriptor ** memory);
    virtual IOExternalMethod * getTargetAndMethodForIndex(
                                         IOService ** target, UInt32 index);
};

#endif /* PPCOSX_FRAMEBUFFER_H */
