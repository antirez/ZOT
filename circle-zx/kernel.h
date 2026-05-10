#ifndef ZX_CIRCLE_KERNEL_H
#define ZX_CIRCLE_KERNEL_H

#include <circle/actled.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/font.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/fs/fat/fatfs.h>
#include <SDCard/emmc.h>

extern "C" {
#include "../spectrum.h"
#include "../tzx.h"
}

enum TShutdownMode
{
    ShutdownNone,
    ShutdownHalt,
    ShutdownReboot
};

class CKernel
{
public:
    CKernel(void);
    ~CKernel(void);

    boolean Initialize(void);
    TShutdownMode Run(void);

private:
    static const unsigned MaxSnapshots = 128;
    static const unsigned SnapshotBufferSize = 512 * 1024;
    static const unsigned TapeBufferSize = 4 * 1024 * 1024;
    static const unsigned OSDVisibleRows = 12;

    static void KeyboardRawHandler(unsigned char modifiers, const unsigned char raw_keys[6]);
    static void KeyboardRemovedHandler(CDevice *pDevice, void *pContext);

    void ApplyInputReport(void);
    void ResetSpectrum(void);
    void BlitSpectrumFramebuffer(void);
    void RenderOSD(void);
    void SetZXKeyState(int row, int bit, boolean pressed);
    void SetZXJoyState(unsigned mask, boolean pressed);
    void ToggleOSD(void);
    void ReloadSnapshots(void);
    boolean LoadTape(unsigned index);
    boolean LoadSnapshot(unsigned index);
    static boolean HasRawKey(const unsigned char raw_keys[6], unsigned char key);
    static boolean HasZ80Extension(const char *name);
    static boolean HasTapeExtension(const char *name);

private:
    CActLED m_ActLED;
    CKernelOptions m_Options;
    CDeviceNameService m_DeviceNameService;
    CScreenDevice m_Screen;
    CSerialDevice m_Serial;
    CExceptionHandler m_ExceptionHandler;
    CInterruptSystem m_Interrupt;
    CTimer m_Timer;
    CLogger m_Logger;
    CPWMSoundBaseDevice m_SoundOut;
    CEMMCDevice m_EMMC;
    CUSBHCIDevice m_USBHCI;
    CFATFileSystem m_FileSystem;

    CUSBKeyboardDevice *volatile m_pKeyboard;
    volatile TShutdownMode m_ShutdownMode;

    ZXSpectrum m_ZX;
    uint8_t m_Framebuffer[ZX_FB_WIDTH * ZX_FB_HEIGHT * 3];
    unsigned m_DrawX;
    unsigned m_DrawY;
    unsigned m_Scale;

    volatile unsigned m_InputSequence;
    volatile unsigned char m_InputModifiers;
    volatile unsigned char m_InputKeys[6];
    unsigned char m_LastInputKeys[6];
    unsigned m_AppliedSequence;

    boolean m_KeyPressed[8][5];
    unsigned char m_JoyPressed;
    boolean m_F2Pressed;
    volatile boolean m_ResetRequested;
    boolean m_F1Pressed;
    boolean m_OsdActive;
    boolean m_OsdDirty;
    boolean m_FileSystemMounted;
    unsigned m_SnapshotCount;
    unsigned m_SelectedSnapshot;
    unsigned m_OsdTopRow;
    char m_SnapshotNames[MaxSnapshots][FS_TITLE_LEN + 1];
    char m_OsdStatus[64];
    uint8_t *m_pSnapshotBuffer;
    uint8_t *m_pTapeBuffer;
    TZXPlayer m_Tape;
    boolean m_TapeLoaded;

    boolean m_F3Pressed;
    boolean m_F4Pressed;
    boolean m_F6Pressed;
    boolean m_FastTapeMode;
    unsigned m_TurboFrameCounter;
    boolean m_AudioActive;

    static CKernel *s_pThis;
};

#endif
