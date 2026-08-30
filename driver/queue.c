/* SPDX-License-Identifier: Apache-2.0 */
/*
 * queue.c - I/O queue and IOCTL dispatch for tt-wind.
 *
 * A single sequential default queue handles IOCTL_TTWIND_* requests.
 * Sequential dispatch keeps the handlers trivially race-free, which is
 * plenty for the low-rate control-plane traffic this interface carries
 * (bulk data moves through user-mapped BARs, never through IOCTLs).
 */

#include "ttwind.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, TtWindQueueInitialize)
#endif

/*
 * TtWindQueueInitialize - create the default I/O queue.
 *
 * Only DeviceControl is handled; create/close are managed by the
 * framework defaults and read/write are not part of the interface (the
 * Linux driver rejects them likewise).
 */
NTSTATUS
TtWindQueueInitialize(
    _In_ WDFDEVICE Device
    )
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    NTSTATUS status;

    PAGED_CODE();

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                           WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = TtWindEvtIoDeviceControl;

    status = WdfIoQueueCreate(Device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: WdfIoQueueCreate failed 0x%08X\n", status));
    }

    return status;
}

/*
 * Handler for IOCTL_TTWIND_GET_DEVICE_INFO.
 *
 * Copies the identity and BAR table recorded at PrepareHardware into the
 * caller's buffer. Requires the full output struct; a short buffer is
 * rejected rather than truncated so the wire format can only evolve by
 * versioned extension.
 */
static NTSTATUS
TtWindGetDeviceInfo(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    const TTWIND_DEVICE_CONTEXT *ctx = TtWindGetDeviceContext(Device);
    TTWIND_DEVICE_INFO_OUT *out;
    UINT32 i;
    NTSTATUS status;

    *BytesWritten = 0;

    status = WdfRequestRetrieveOutputBuffer(Request,
                                            sizeof(*out),
                                            (PVOID *)&out,
                                            NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(out, sizeof(*out));

    out->VendorId = ctx->VendorId;
    out->DeviceId = ctx->DeviceId;
    out->SubsystemVendorId = ctx->SubsystemVendorId;
    out->SubsystemId = ctx->SubsystemId;
    out->Bus = ctx->Bus;
    out->Device = ctx->Device;
    out->Function = ctx->Function;
    out->PciDomain = ctx->PciDomain;
    out->BarCount = ctx->BarCount;

    for (i = 0; i < ctx->BarCount && i < TTWIND_MAX_BARS; i++) {
        out->Bars[i].Phys = (unsigned __int64)ctx->Bars[i].Phys.QuadPart;
        out->Bars[i].Size = ctx->Bars[i].Size;
    }

    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}

/*
 * TtWindEvtIoDeviceControl - IOCTL dispatch.
 */
VOID
TtWindEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    size_t bytesWritten = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    /*
     * Reset-generation fence: a handle opened before the most recent
     * armed reset is stale - the reset reclaimed its TLB windows and
     * invalidated its view of the device. Every ioctl from it fails as
     * device-gone; the handle that issued the reset was carried forward
     * to the current generation and passes. Reading the generation and
     * NeedsHwInit without a lock is safe: both are only written by the
     * reset ioctls, which run on this same sequential queue.
     */
    if (fileObject != NULL &&
        TtWindGetFileContext(fileObject)->ResetGeneration !=
            ctx->ResetGeneration) {
        WdfRequestComplete(Request, STATUS_DEVICE_REMOVED);
        return;
    }

    /*
     * Restricted state (armed reset not yet recovered, see reset.c):
     * the device may be off the bus, and on this platform an MMIO read
     * during link-down stalls the machine (incident 2026-08-30). Only
     * the allow-list below may proceed; everything else - anything that
     * could touch MMIO or hand out a mapping of it - fails BEFORE
     * touching hardware, with a distinct, retryable status. Recovery:
     * poll IOCTL_TTWIND_POST_RESET.
     */
    if (ctx->NeedsHwInit &&
        IoControlCode != IOCTL_TTWIND_GET_DEVICE_INFO &&
        IoControlCode != IOCTL_TTWIND_QUERY_SYSMEM &&
        IoControlCode != IOCTL_TTWIND_RESET_DEVICE &&
        IoControlCode != IOCTL_TTWIND_POST_RESET) {
        WdfRequestComplete(Request, STATUS_REINITIALIZATION_NEEDED);
        return;
    }

    switch (IoControlCode) {
    case IOCTL_TTWIND_GET_DEVICE_INFO:
        status = TtWindGetDeviceInfo(device, Request, &bytesWritten);
        break;

    case IOCTL_TTWIND_MAP_BAR:
        status = TtWindIoctlMapBar(device, Request, &bytesWritten);
        break;

    case IOCTL_TTWIND_UNMAP_BAR:
        status = TtWindIoctlUnmapBar(device, Request);
        break;

    case IOCTL_TTWIND_ALLOCATE_TLB:
        status = TtWindIoctlAllocateTlb(device, Request, &bytesWritten);
        break;

    case IOCTL_TTWIND_FREE_TLB:
        status = TtWindIoctlFreeTlb(device, Request);
        break;

    case IOCTL_TTWIND_CONFIGURE_TLB:
        status = TtWindIoctlConfigureTlb(device, Request);
        break;

    case IOCTL_TTWIND_MAP_TLB:
        status = TtWindIoctlMapTlb(device, Request, &bytesWritten);
        break;

    case IOCTL_TTWIND_SMC_MSG:
        status = TtWindIoctlSmcMsg(device, Request, &bytesWritten);
        break;

    case IOCTL_TTWIND_RESET_DEVICE:
        status = TtWindIoctlResetDevice(device, Request);
        break;

    case IOCTL_TTWIND_POST_RESET:
        status = TtWindIoctlPostReset(device, Request);
        break;

    case IOCTL_TTWIND_QUERY_SYSMEM:
        status = TtWindIoctlQuerySysmem(device, Request, &bytesWritten);
        break;

    case IOCTL_TTWIND_MAP_SYSMEM:
        status = TtWindIoctlMapSysmem(device, Request, &bytesWritten);
        break;

    case IOCTL_TTWIND_ARC_STATUS:
        status = TtWindIoctlArcStatus(device, Request, &bytesWritten);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesWritten);
}
