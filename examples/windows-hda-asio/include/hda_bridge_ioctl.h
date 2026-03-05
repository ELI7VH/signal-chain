/*
 * hda_bridge_ioctl.h -- Shared IOCTL definitions for HDA ASIO Bridge
 *
 * Used by both the kernel driver (hda_bridge.sys) and user-mode DLL (hda_asio.dll).
 * The kernel driver's ONLY job is bridging the WaveRT DMA buffer allocation
 * that requires kernel mode -- everything else happens in user mode.
 *
 * Architecture:
 *   User mode: enumerate devices, open HDA filter, create KS pins
 *   Kernel bridge: allocate WaveRT DMA buffer, map to user, set pin states
 *   User mode: read/write DMA buffer directly (zero-copy, zero-syscall)
 */

#ifndef HDA_BRIDGE_IOCTL_H
#define HDA_BRIDGE_IOCTL_H

/* Device names */
#define HDA_BRIDGE_DEVICE_NAME_W   L"\\Device\\HdaAsioBridge"
#define HDA_BRIDGE_SYMLINK_NAME_W  L"\\DosDevices\\HdaAsioBridge"
#define HDA_BRIDGE_USER_PATH       "\\\\.\\HdaAsioBridge"
#define HDA_BRIDGE_USER_PATH_W     L"\\\\.\\HdaAsioBridge"

/* Custom device type for CTL_CODE */
#define FILE_DEVICE_HDA_BRIDGE  0x8000

/*
 * IOCTL_HDA_ALLOC_RT_BUFFER
 *   Allocates the WaveRT DMA buffer for a KS pin and maps it into the
 *   calling process's address space. This is the one operation that
 *   requires kernel mode.
 *
 *   Input:  HDA_ALLOC_BUFFER_IN
 *   Output: HDA_ALLOC_BUFFER_OUT
 */
#define IOCTL_HDA_ALLOC_RT_BUFFER \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_HDA_FREE_RT_BUFFER
 *   Unmaps and releases the DMA buffer for a pin.
 *
 *   Input:  HDA_FREE_BUFFER_IN
 *   Output: none
 */
#define IOCTL_HDA_FREE_RT_BUFFER \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_HDA_SET_PIN_STATE
 *   Transitions a KS pin through states (STOP -> ACQUIRE -> PAUSE -> RUN).
 *   May need kernel mode because PortCls checks requestor mode on some paths.
 *
 *   Input:  HDA_SET_STATE_IN
 *   Output: none
 */
#define IOCTL_HDA_SET_PIN_STATE \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_HDA_GET_RT_POSITION
 *   Maps the WaveRT hardware position register into user mode.
 *   After this call, user code can read the DMA position without any syscall.
 *
 *   Input:  HDA_POSITION_MAP_IN
 *   Output: HDA_POSITION_MAP_OUT
 */
#define IOCTL_HDA_GET_RT_POSITION \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---- Structures ---- */

#ifdef _NTDDK_
/* Kernel mode -- types come from ntddk.h */
#else
/* User mode -- types come from windows.h */
#ifndef _WINDOWS_
#include <windows.h>
#endif
#include <winioctl.h>
#endif

/*
 * Input for IOCTL_HDA_ALLOC_RT_BUFFER.
 * PinHandle is the user-mode handle returned by KsCreatePin.
 */
typedef struct _HDA_ALLOC_BUFFER_IN {
    ULONG_PTR   PinHandle;          /* User-mode KS pin handle */
    ULONG       RequestedSize;      /* Desired buffer size in bytes (0 = default) */
} HDA_ALLOC_BUFFER_IN;

/*
 * Output for IOCTL_HDA_ALLOC_RT_BUFFER.
 * BufferAddress is a user-mode virtual address -- write/read directly.
 */
typedef struct _HDA_ALLOC_BUFFER_OUT {
    ULONG_PTR   BufferAddress;      /* User VA of mapped DMA buffer */
    ULONG       BufferSize;         /* Actual buffer size in bytes */
} HDA_ALLOC_BUFFER_OUT;

/* Input for IOCTL_HDA_FREE_RT_BUFFER */
typedef struct _HDA_FREE_BUFFER_IN {
    ULONG_PTR   PinHandle;          /* Same pin handle used for alloc */
} HDA_FREE_BUFFER_IN;

/* KSSTATE values (matching ks.h) */
#define HDA_PIN_STATE_STOP      0
#define HDA_PIN_STATE_ACQUIRE   1
#define HDA_PIN_STATE_PAUSE     2
#define HDA_PIN_STATE_RUN       3

/* Input for IOCTL_HDA_SET_PIN_STATE */
typedef struct _HDA_SET_STATE_IN {
    ULONG_PTR   PinHandle;          /* User-mode KS pin handle */
    ULONG       State;              /* HDA_PIN_STATE_xxx */
} HDA_SET_STATE_IN;

/* Input for IOCTL_HDA_GET_RT_POSITION */
typedef struct _HDA_POSITION_MAP_IN {
    ULONG_PTR   PinHandle;          /* User-mode KS pin handle */
} HDA_POSITION_MAP_IN;

/*
 * Output for IOCTL_HDA_GET_RT_POSITION.
 * PositionRegister is a user-mode pointer to a ULONG that the hardware
 * updates in real time with the current DMA byte offset. Read it directly
 * from user mode -- no syscall needed.
 */
typedef struct _HDA_POSITION_MAP_OUT {
    ULONG_PTR   PositionRegister;   /* User VA of hardware position register */
} HDA_POSITION_MAP_OUT;

/* Version for compatibility checks */
#define HDA_BRIDGE_VERSION  0x0001

#endif /* HDA_BRIDGE_IOCTL_H */
