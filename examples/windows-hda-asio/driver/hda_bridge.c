/*
 * hda_bridge.c -- Kernel bridge driver for WaveRT DMA buffer access
 *
 * signal-chain experiment: custom kernel driver for Conexant CX20753/4
 *
 * WHAT THIS DRIVER DOES:
 *   1. Receives a KS pin handle from user mode (pin already created by user)
 *   2. Calls KSPROPERTY_RTAUDIO_BUFFER from kernel context (the ONE operation
 *      that requires kernel mode on WaveRT)
 *   3. Maps the DMA buffer into the calling process's address space
 *   4. Transitions pin states (STOP -> ACQUIRE -> PAUSE -> RUN)
 *
 * WHAT THIS DRIVER DOES NOT DO:
 *   - Device enumeration (user mode does this)
 *   - Filter/pin creation (user mode does this via KsCreatePin)
 *   - Audio processing (user mode reads/writes DMA buffer directly)
 *   - Position tracking (hardware register mapped to user mode)
 *
 * The hot path is ENTIRELY in user mode: zero-copy, zero-syscall.
 * This driver is only called during setup and teardown.
 *
 * Build with MinGW (w64devkit) -- no WDK needed:
 *   gcc -nostdlib -nostartfiles -shared -o hda_bridge.sys hda_bridge.c
 *       -I<w64devkit>/include/ddk -Wl,--subsystem,native -Wl,--entry,DriverEntry
 *       -lntoskrnl -lhal -lks -lksguid
 */

#include <ntddk.h>
#include <ks.h>

/*
 * We do NOT include <ksmedia.h> because the w64devkit version pulls in
 * user-mode types (FLOAT, BYTE, DWORD, RECT) that don't exist in kernel mode.
 * ks.h already provides KSPROPSETID_Connection, KSSTATE, and IOCTL_KS_PROPERTY.
 */

/* ---- IOCTL definitions (must match hda_bridge_ioctl.h) ---- */

#define FILE_DEVICE_HDA_BRIDGE  0x8000

#define IOCTL_HDA_ALLOC_RT_BUFFER \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HDA_FREE_RT_BUFFER \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HDA_SET_PIN_STATE \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HDA_GET_RT_POSITION \
    CTL_CODE(FILE_DEVICE_HDA_BRIDGE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Shared structures (must match hda_bridge_ioctl.h) */
typedef struct {
    ULONG_PTR   PinHandle;
    ULONG       RequestedSize;
} HDA_ALLOC_BUFFER_IN;

typedef struct {
    ULONG_PTR   BufferAddress;
    ULONG       BufferSize;
} HDA_ALLOC_BUFFER_OUT;

typedef struct {
    ULONG_PTR   PinHandle;
} HDA_FREE_BUFFER_IN;

typedef struct {
    ULONG_PTR   PinHandle;
    ULONG       State;
} HDA_SET_STATE_IN;

typedef struct {
    ULONG_PTR   PinHandle;
} HDA_POSITION_MAP_IN;

typedef struct {
    ULONG_PTR   PositionRegister;
} HDA_POSITION_MAP_OUT;

/* ---- RtAudio property definitions ---- */

/* KSPROPSETID_RtAudio = {A855A48C-2F78-4729-9051-1968746B9EEF} */
static const GUID KSPROPSETID_RtAudio_Local = {
    0xA855A48CL, 0x2F78, 0x4729,
    { 0x90, 0x51, 0x19, 0x68, 0x74, 0x6B, 0x9E, 0xEF }
};

#define KSPROPERTY_RTAUDIO_BUFFER           0
#define KSPROPERTY_RTAUDIO_HWLATENCY        1
#define KSPROPERTY_RTAUDIO_POSITIONREGISTER 2

/* KSRTAUDIO_BUFFER_PROPERTY -- input for KSPROPERTY_RTAUDIO_BUFFER */
typedef struct {
    KSPROPERTY  Property;
    PVOID       BaseAddress;
    ULONG       RequestedBufferSize;
} KSRTAUDIO_BUFFER_PROPERTY_LOCAL;

/* KSRTAUDIO_BUFFER -- output for KSPROPERTY_RTAUDIO_BUFFER */
typedef struct {
    PVOID   BufferAddress;
    ULONG   BufferSize;
    PVOID   CallMemoryBarrier;
} KSRTAUDIO_BUFFER_LOCAL;

/* KSRTAUDIO_HWLATENCY -- output for KSPROPERTY_RTAUDIO_HWLATENCY */
typedef struct {
    ULONG   FifoSize;
    ULONG   ChipsetDelay;
    ULONG   CodecDelay;
} KSRTAUDIO_HWLATENCY_LOCAL;

/* ---- Device extension ---- */

#define MAX_PIN_MAPPINGS    4   /* capture L/R + render L/R */

typedef struct _PIN_MAPPING {
    BOOLEAN             InUse;
    PFILE_OBJECT        PinFileObject;      /* Referenced file object for KS pin */
    PVOID               DmaBufferKernelVa;  /* Kernel VA from RTAUDIO_BUFFER */
    ULONG               DmaBufferSize;
    PMDL                DmaBufferMdl;       /* MDL for user-mode mapping */
    PVOID               DmaBufferUserVa;    /* User VA (mapped) */
    PVOID               PosRegKernelVa;     /* Position register kernel VA */
    PMDL                PosRegMdl;          /* MDL for position register */
    PVOID               PosRegUserVa;       /* Position register user VA */
} PIN_MAPPING;

typedef struct _DEVICE_EXTENSION {
    PDEVICE_OBJECT      DeviceObject;
    UNICODE_STRING      SymlinkName;
    PIN_MAPPING         Pins[MAX_PIN_MAPPINGS];
    FAST_MUTEX          Lock;
} DEVICE_EXTENSION;

/* ---- Forward declarations ---- */

DRIVER_INITIALIZE   DriverEntry;
DRIVER_UNLOAD       DriverUnload;
DRIVER_DISPATCH     DispatchCreate;
DRIVER_DISPATCH     DispatchClose;
DRIVER_DISPATCH     DispatchDeviceControl;

/* Tags for pool allocations */
#define TAG_HDA     'AADH'

/* ---- IRP completion helper ---- */

static NTSTATUS
KsPropertyCompletion(
    PDEVICE_OBJECT  DeviceObject,
    PIRP            Irp,
    PVOID           Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ---- Send a KS property request to a pin's device ---- */

static NTSTATUS
SendKsProperty(
    PFILE_OBJECT    FileObject,
    PVOID           PropertyBuffer,
    ULONG           PropertySize,
    PVOID           ResultBuffer,
    ULONG           ResultSize,
    PULONG          BytesReturned
)
{
    PDEVICE_OBJECT  deviceObject;
    PIRP            irp;
    KEVENT          event;
    NTSTATUS        status;
    PIO_STACK_LOCATION stack;

    deviceObject = IoGetRelatedDeviceObject(FileObject);
    if (!deviceObject) return STATUS_INVALID_DEVICE_REQUEST;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoAllocateIrp(deviceObject->StackSize, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    /* Set up IRP for METHOD_NEITHER IOCTL */
    irp->RequestorMode = KernelMode;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    irp->Tail.Overlay.OriginalFileObject = FileObject;
    irp->UserBuffer = ResultBuffer;

    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    stack->MinorFunction = 0;
    stack->FileObject = FileObject;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_KS_PROPERTY;
    stack->Parameters.DeviceIoControl.InputBufferLength = PropertySize;
    stack->Parameters.DeviceIoControl.OutputBufferLength = ResultSize;
    stack->Parameters.DeviceIoControl.Type3InputBuffer = PropertyBuffer;

    IoSetCompletionRoutine(irp, KsPropertyCompletion, &event, TRUE, TRUE, TRUE);

    status = IoCallDriver(deviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }

    status = irp->IoStatus.Status;
    if (BytesReturned) {
        *BytesReturned = (ULONG)irp->IoStatus.Information;
    }

    IoFreeIrp(irp);
    return status;
}

/* ---- Find or allocate a pin mapping slot ---- */

static PIN_MAPPING *
FindPinMapping(DEVICE_EXTENSION *devExt, PFILE_OBJECT pinFileObject)
{
    ULONG i;
    for (i = 0; i < MAX_PIN_MAPPINGS; i++) {
        if (devExt->Pins[i].InUse &&
            devExt->Pins[i].PinFileObject == pinFileObject) {
            return &devExt->Pins[i];
        }
    }
    return NULL;
}

static PIN_MAPPING *
AllocPinMapping(DEVICE_EXTENSION *devExt)
{
    ULONG i;
    for (i = 0; i < MAX_PIN_MAPPINGS; i++) {
        if (!devExt->Pins[i].InUse) {
            RtlZeroMemory(&devExt->Pins[i], sizeof(PIN_MAPPING));
            devExt->Pins[i].InUse = TRUE;
            return &devExt->Pins[i];
        }
    }
    return NULL;
}

/* ---- Free a pin mapping (unmap buffers, release references) ---- */

static void
FreePinMapping(PIN_MAPPING *pin)
{
    if (!pin->InUse) return;

    /* Unmap DMA buffer from user mode */
    if (pin->DmaBufferUserVa && pin->DmaBufferMdl) {
        MmUnmapLockedPages(pin->DmaBufferUserVa, pin->DmaBufferMdl);
        pin->DmaBufferUserVa = NULL;
    }
    if (pin->DmaBufferMdl) {
        IoFreeMdl(pin->DmaBufferMdl);
        pin->DmaBufferMdl = NULL;
    }

    /* Unmap position register from user mode */
    if (pin->PosRegUserVa && pin->PosRegMdl) {
        MmUnmapLockedPages(pin->PosRegUserVa, pin->PosRegMdl);
        pin->PosRegUserVa = NULL;
    }
    if (pin->PosRegMdl) {
        IoFreeMdl(pin->PosRegMdl);
        pin->PosRegMdl = NULL;
    }

    /* Release file object reference */
    if (pin->PinFileObject) {
        ObDereferenceObject(pin->PinFileObject);
        pin->PinFileObject = NULL;
    }

    pin->InUse = FALSE;
}

/* ---- IOCTL: Allocate WaveRT DMA buffer ---- */

static NTSTATUS
HandleAllocRtBuffer(
    DEVICE_EXTENSION   *devExt,
    HDA_ALLOC_BUFFER_IN *in,
    HDA_ALLOC_BUFFER_OUT *out
)
{
    NTSTATUS        status;
    PFILE_OBJECT    pinFileObject = NULL;
    PIN_MAPPING    *pin;
    KSRTAUDIO_BUFFER_PROPERTY_LOCAL bufferProp;
    KSRTAUDIO_BUFFER_LOCAL          bufferResult;
    ULONG           bytesReturned;

    RtlZeroMemory(&bufferResult, sizeof(bufferResult));

    /* Get file object from the user's KS pin handle */
    status = ObReferenceObjectByHandle(
        (HANDLE)in->PinHandle,
        FILE_READ_DATA | FILE_WRITE_DATA,
        *IoFileObjectType,
        UserMode,           /* Handle came from user mode */
        (PVOID *)&pinFileObject,
        NULL
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("HdaBridge: ObReferenceObjectByHandle failed: 0x%08X\n", status));
        return status;
    }

    /* Check if we already have a mapping for this pin */
    ExAcquireFastMutex(&devExt->Lock);
    pin = FindPinMapping(devExt, pinFileObject);
    if (pin) {
        /* Already allocated -- return existing mapping */
        out->BufferAddress = (ULONG_PTR)pin->DmaBufferUserVa;
        out->BufferSize = pin->DmaBufferSize;
        ExReleaseFastMutex(&devExt->Lock);
        ObDereferenceObject(pinFileObject);
        return STATUS_SUCCESS;
    }

    /* Allocate a new slot */
    pin = AllocPinMapping(devExt);
    if (!pin) {
        ExReleaseFastMutex(&devExt->Lock);
        ObDereferenceObject(pinFileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pin->PinFileObject = pinFileObject;  /* Transfer reference ownership */
    ExReleaseFastMutex(&devExt->Lock);

    /* Build KSPROPERTY_RTAUDIO_BUFFER request */
    RtlZeroMemory(&bufferProp, sizeof(bufferProp));
    bufferProp.Property.Set = KSPROPSETID_RtAudio_Local;
    bufferProp.Property.Id = KSPROPERTY_RTAUDIO_BUFFER;
    bufferProp.Property.Flags = KSPROPERTY_TYPE_GET;
    bufferProp.BaseAddress = NULL;      /* Let the driver choose */
    bufferProp.RequestedBufferSize = in->RequestedSize > 0
        ? in->RequestedSize
        : 160 * 4 * 2;                 /* Default: 160 frames * 4 bytes/frame * 2 (double buffer) */

    KdPrint(("HdaBridge: Requesting RT buffer, size=%u\n", bufferProp.RequestedBufferSize));

    /* THIS IS THE CRITICAL CALL -- requires kernel mode */
    status = SendKsProperty(
        pinFileObject,
        &bufferProp, sizeof(bufferProp),
        &bufferResult, sizeof(bufferResult),
        &bytesReturned
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("HdaBridge: KSPROPERTY_RTAUDIO_BUFFER failed: 0x%08X\n", status));
        goto fail;
    }

    KdPrint(("HdaBridge: RT buffer allocated: addr=%p, size=%u\n",
             bufferResult.BufferAddress, bufferResult.BufferSize));

    pin->DmaBufferKernelVa = bufferResult.BufferAddress;
    pin->DmaBufferSize = bufferResult.BufferSize;

    /* Create MDL for the DMA buffer and map to user process */
    pin->DmaBufferMdl = IoAllocateMdl(
        bufferResult.BufferAddress,
        bufferResult.BufferSize,
        FALSE, FALSE, NULL
    );
    if (!pin->DmaBufferMdl) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    MmBuildMdlForNonPagedPool(pin->DmaBufferMdl);

    /*
     * MmMapLockedPagesSpecifyCache can raise exceptions on failure.
     * MinGW doesn't support MSVC's __try/__except, so we call directly.
     * In practice, this should not fail on valid non-paged MDLs.
     */
    pin->DmaBufferUserVa = MmMapLockedPagesSpecifyCache(
        pin->DmaBufferMdl,
        UserMode,
        MmNonCached,
        NULL,               /* Let MM choose the user VA */
        FALSE,
        NormalPagePriority
    );

    if (!pin->DmaBufferUserVa) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        KdPrint(("HdaBridge: MmMapLockedPages returned NULL\n"));
        goto fail;
    }

    KdPrint(("HdaBridge: Buffer mapped to user VA: %p\n", pin->DmaBufferUserVa));

    /* Return the user-mode mapping */
    out->BufferAddress = (ULONG_PTR)pin->DmaBufferUserVa;
    out->BufferSize = pin->DmaBufferSize;
    return STATUS_SUCCESS;

fail:
    ExAcquireFastMutex(&devExt->Lock);
    FreePinMapping(pin);
    ExReleaseFastMutex(&devExt->Lock);
    return status;
}

/* ---- IOCTL: Free WaveRT DMA buffer ---- */

static NTSTATUS
HandleFreeRtBuffer(
    DEVICE_EXTENSION   *devExt,
    HDA_FREE_BUFFER_IN *in
)
{
    NTSTATUS        status;
    PFILE_OBJECT    pinFileObject = NULL;
    PIN_MAPPING    *pin;

    status = ObReferenceObjectByHandle(
        (HANDLE)in->PinHandle,
        FILE_READ_DATA,
        *IoFileObjectType,
        UserMode,
        (PVOID *)&pinFileObject,
        NULL
    );
    if (!NT_SUCCESS(status)) return status;

    ExAcquireFastMutex(&devExt->Lock);
    pin = FindPinMapping(devExt, pinFileObject);
    if (pin) {
        FreePinMapping(pin);
        status = STATUS_SUCCESS;
    } else {
        status = STATUS_NOT_FOUND;
    }
    ExReleaseFastMutex(&devExt->Lock);

    ObDereferenceObject(pinFileObject);
    return status;
}

/* ---- IOCTL: Set pin state ---- */

static NTSTATUS
HandleSetPinState(
    DEVICE_EXTENSION   *devExt,
    HDA_SET_STATE_IN   *in
)
{
    NTSTATUS        status;
    PFILE_OBJECT    pinFileObject = NULL;

    /* Validate state value */
    if (in->State > 3) return STATUS_INVALID_PARAMETER;  /* KSSTATE_RUN = 3 */

    status = ObReferenceObjectByHandle(
        (HANDLE)in->PinHandle,
        FILE_READ_DATA | FILE_WRITE_DATA,
        *IoFileObjectType,
        UserMode,
        (PVOID *)&pinFileObject,
        NULL
    );
    if (!NT_SUCCESS(status)) return status;

    /* Build KSPROPERTY_CONNECTION_STATE SET request */
    struct {
        KSPROPERTY  Property;
        KSSTATE     State;
    } stateReq;

    RtlZeroMemory(&stateReq, sizeof(stateReq));
    stateReq.Property.Set = KSPROPSETID_Connection;
    stateReq.Property.Id = KSPROPERTY_CONNECTION_STATE;
    stateReq.Property.Flags = KSPROPERTY_TYPE_SET;
    stateReq.State = (KSSTATE)in->State;

    KdPrint(("HdaBridge: Setting pin state to %u\n", in->State));

    status = SendKsProperty(
        pinFileObject,
        &stateReq, sizeof(stateReq),
        NULL, 0,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("HdaBridge: Set pin state failed: 0x%08X\n", status));
    }

    ObDereferenceObject(pinFileObject);
    return status;
}

/* ---- IOCTL: Map position register to user mode ---- */

static NTSTATUS
HandleGetRtPosition(
    DEVICE_EXTENSION    *devExt,
    HDA_POSITION_MAP_IN *in,
    HDA_POSITION_MAP_OUT *out
)
{
    NTSTATUS        status;
    PFILE_OBJECT    pinFileObject = NULL;
    PIN_MAPPING    *pin;

    status = ObReferenceObjectByHandle(
        (HANDLE)in->PinHandle,
        FILE_READ_DATA,
        *IoFileObjectType,
        UserMode,
        (PVOID *)&pinFileObject,
        NULL
    );
    if (!NT_SUCCESS(status)) return status;

    /* Find existing pin mapping */
    ExAcquireFastMutex(&devExt->Lock);
    pin = FindPinMapping(devExt, pinFileObject);
    ExReleaseFastMutex(&devExt->Lock);

    if (!pin) {
        ObDereferenceObject(pinFileObject);
        return STATUS_NOT_FOUND;
    }

    /* If already mapped, return existing */
    if (pin->PosRegUserVa) {
        out->PositionRegister = (ULONG_PTR)pin->PosRegUserVa;
        ObDereferenceObject(pinFileObject);
        return STATUS_SUCCESS;
    }

    /* Request position register from WaveRT */
    KSPROPERTY posReq;
    RtlZeroMemory(&posReq, sizeof(posReq));
    posReq.Set = KSPROPSETID_RtAudio_Local;
    posReq.Id = KSPROPERTY_RTAUDIO_POSITIONREGISTER;
    posReq.Flags = KSPROPERTY_TYPE_GET;

    struct {
        PVOID   Register;
    } posResult;
    RtlZeroMemory(&posResult, sizeof(posResult));

    ULONG bytesReturned;
    status = SendKsProperty(
        pinFileObject,
        &posReq, sizeof(posReq),
        &posResult, sizeof(posResult),
        &bytesReturned
    );

    if (!NT_SUCCESS(status)) {
        /*
         * Position register mapping is optional -- some drivers don't support it.
         * The ASIO DLL can fall back to KSPROPERTY_AUDIO_POSITION via IOCTL.
         */
        KdPrint(("HdaBridge: Position register not available: 0x%08X\n", status));
        out->PositionRegister = 0;
        ObDereferenceObject(pinFileObject);
        return STATUS_SUCCESS;  /* Not a fatal error */
    }

    pin->PosRegKernelVa = posResult.Register;

    /* Map position register page to user mode */
    pin->PosRegMdl = IoAllocateMdl(posResult.Register, sizeof(ULONG), FALSE, FALSE, NULL);
    if (!pin->PosRegMdl) {
        out->PositionRegister = 0;
        ObDereferenceObject(pinFileObject);
        return STATUS_SUCCESS;
    }

    MmBuildMdlForNonPagedPool(pin->PosRegMdl);

    pin->PosRegUserVa = MmMapLockedPagesSpecifyCache(
        pin->PosRegMdl,
        UserMode,
        MmNonCached,
        NULL,
        FALSE,
        NormalPagePriority
    );

    if (!pin->PosRegUserVa) {
        IoFreeMdl(pin->PosRegMdl);
        pin->PosRegMdl = NULL;
    }

    out->PositionRegister = (ULONG_PTR)pin->PosRegUserVa;
    ObDereferenceObject(pinFileObject);
    return STATUS_SUCCESS;
}

/* ---- Dispatch routines ---- */

NTSTATUS
DispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
DispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    DEVICE_EXTENSION *devExt = (DEVICE_EXTENSION *)DeviceObject->DeviceExtension;
    ULONG i;

    /* Clean up all pin mappings when the user closes the device */
    ExAcquireFastMutex(&devExt->Lock);
    for (i = 0; i < MAX_PIN_MAPPINGS; i++) {
        if (devExt->Pins[i].InUse) {
            FreePinMapping(&devExt->Pins[i]);
        }
    }
    ExReleaseFastMutex(&devExt->Lock);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    DEVICE_EXTENSION       *devExt = (DEVICE_EXTENSION *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION      stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG                   ioctl = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG                   inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG                   outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID                   buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS                status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG                   info = 0;

    switch (ioctl) {

    case IOCTL_HDA_ALLOC_RT_BUFFER:
        if (inLen < sizeof(HDA_ALLOC_BUFFER_IN) || outLen < sizeof(HDA_ALLOC_BUFFER_OUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            HDA_ALLOC_BUFFER_IN  *in = (HDA_ALLOC_BUFFER_IN *)buffer;
            HDA_ALLOC_BUFFER_OUT  out;
            RtlZeroMemory(&out, sizeof(out));

            status = HandleAllocRtBuffer(devExt, in, &out);
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(buffer, &out, sizeof(out));
                info = sizeof(HDA_ALLOC_BUFFER_OUT);
            }
        }
        break;

    case IOCTL_HDA_FREE_RT_BUFFER:
        if (inLen < sizeof(HDA_FREE_BUFFER_IN)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = HandleFreeRtBuffer(devExt, (HDA_FREE_BUFFER_IN *)buffer);
        break;

    case IOCTL_HDA_SET_PIN_STATE:
        if (inLen < sizeof(HDA_SET_STATE_IN)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = HandleSetPinState(devExt, (HDA_SET_STATE_IN *)buffer);
        break;

    case IOCTL_HDA_GET_RT_POSITION:
        if (inLen < sizeof(HDA_POSITION_MAP_IN) || outLen < sizeof(HDA_POSITION_MAP_OUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            HDA_POSITION_MAP_IN  *in = (HDA_POSITION_MAP_IN *)buffer;
            HDA_POSITION_MAP_OUT  out;
            RtlZeroMemory(&out, sizeof(out));

            status = HandleGetRtPosition(devExt, in, &out);
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(buffer, &out, sizeof(out));
                info = sizeof(HDA_POSITION_MAP_OUT);
            }
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ---- Driver unload ---- */

void
DriverUnload(PDRIVER_OBJECT DriverObject)
{
    DEVICE_EXTENSION *devExt;

    if (DriverObject->DeviceObject) {
        devExt = (DEVICE_EXTENSION *)DriverObject->DeviceObject->DeviceExtension;

        /* Delete symbolic link */
        IoDeleteSymbolicLink(&devExt->SymlinkName);

        /* Clean up any remaining pin mappings */
        {
            ULONG i;
            for (i = 0; i < MAX_PIN_MAPPINGS; i++) {
                if (devExt->Pins[i].InUse) {
                    FreePinMapping(&devExt->Pins[i]);
                }
            }
        }

        IoDeleteDevice(DriverObject->DeviceObject);
    }

    KdPrint(("HdaBridge: Driver unloaded\n"));
}

/* ---- Driver entry ---- */

NTSTATUS
DriverEntry(
    PDRIVER_OBJECT      DriverObject,
    PUNICODE_STRING     RegistryPath
)
{
    NTSTATUS            status;
    PDEVICE_OBJECT      deviceObject = NULL;
    DEVICE_EXTENSION   *devExt;
    UNICODE_STRING      deviceName;
    UNICODE_STRING      symlinkName;

    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrint(("HdaBridge: Driver loading (signal-chain WaveRT bridge)\n"));

    /* Create device object */
    RtlInitUnicodeString(&deviceName, L"\\Device\\HdaAsioBridge");
    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,                  /* Not exclusive -- multiple ASIO apps could open */
        &deviceObject
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("HdaBridge: IoCreateDevice failed: 0x%08X\n", status));
        return status;
    }

    /* Initialize device extension */
    devExt = (DEVICE_EXTENSION *)deviceObject->DeviceExtension;
    RtlZeroMemory(devExt, sizeof(DEVICE_EXTENSION));
    devExt->DeviceObject = deviceObject;
    ExInitializeFastMutex(&devExt->Lock);

    /* Create symbolic link for user-mode access */
    RtlInitUnicodeString(&symlinkName, L"\\DosDevices\\HdaAsioBridge");
    status = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("HdaBridge: IoCreateSymbolicLink failed: 0x%08X\n", status));
        IoDeleteDevice(deviceObject);
        return status;
    }
    devExt->SymlinkName = symlinkName;

    /* Set up dispatch routines */
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    /* Use direct I/O for efficiency */
    deviceObject->Flags |= DO_DIRECT_IO;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    KdPrint(("HdaBridge: Driver loaded successfully\n"));
    KdPrint(("HdaBridge: User-mode access via \\\\.\\HdaAsioBridge\n"));

    return STATUS_SUCCESS;
}
