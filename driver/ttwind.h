/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ttwind.h - private declarations of the tt-wind KMDF driver.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdmguid.h> /* GUID_BUS_INTERFACE_STANDARD */

#include "ttwind_ioctl.h"

#define TTWIND_POOL_TAG 'dnWT' /* "TWnd" in the pool viewer */

/*
 * Blackhole TLB geometry (from tt-kmd blackhole.c). BAR0 starts with 202
 * 2 MiB windows; the TLB configuration registers live at BAR0 offset
 * 0x1FC00000, 12 bytes per window (2 MiB windows first, then the eight
 * 4 GiB windows), followed by one 4-byte strided-multicast register for
 * each of the first 32 2 MiB windows.
 */
#define TTWIND_BH_TLB_2M_COUNT       TTWIND_TLB_2M_WINDOW_COUNT /* 202  */
#define TTWIND_BH_TLB_2M_SHIFT       21
#define TTWIND_BH_TLB_2M_SIZE        TTWIND_TLB_WINDOW_SIZE_2M
#define TTWIND_BH_TLB_4G_COUNT       8u
#define TTWIND_BH_TLB_REG_SIZE       12u
#define TTWIND_BH_TLB_REGS_START     0x1FC00000u /* BAR0 offset          */
#define TTWIND_BH_TLB_REGS_LEN       0x1000u     /* covers all TLB regs  */
#define TTWIND_BH_TLB_STRIDED_COUNT  32u
#define TTWIND_BH_TLB_STRIDED_REG_SIZE 4u
#define TTWIND_BH_TLB_STRIDED_REGS_OFFSET \
    ((TTWIND_BH_TLB_2M_COUNT + TTWIND_BH_TLB_4G_COUNT) * TTWIND_BH_TLB_REG_SIZE)

/*
 * The topmost 2 MiB TLB window is reserved for the kernel's own NOC
 * access (ARC firmware messaging); tt-kmd reserves the same window
 * (blackhole.c KERNEL_TLB_INDEX). It is pre-set in the allocator bitmap
 * so user handles can never allocate or map it.
 */
#define TTWIND_BH_KERNEL_TLB_INDEX  (TTWIND_BH_TLB_2M_COUNT - 1)
#define TTWIND_BH_KERNEL_TLB_START \
    ((UINT64)TTWIND_BH_KERNEL_TLB_INDEX * TTWIND_BH_TLB_2M_SIZE)

/*
 * One ARC (SMC) firmware mailbox message: 8 32-bit words, word 0 is the
 * header (message type in the low byte). Same layout as tt-kmd's
 * struct arc_msg (msgqueue.h).
 */
typedef struct _TTWIND_ARC_MSG {
    UINT32 Header;
    UINT32 Payload[7];
} TTWIND_ARC_MSG, *PTTWIND_ARC_MSG;

/*
 * One live user-mode mapping of device memory (a BAR range or a TLB
 * window). Linked into the device context's MappingList; owned by the
 * file object (handle) that created it and torn down at the latest on
 * that handle's cleanup, or at ReleaseHardware.
 */
typedef struct _TTWIND_USER_MAPPING {
    LIST_ENTRY    ListEntry;
    WDFFILEOBJECT FileObject; /* owning handle (comparison only)         */
    PEPROCESS     Process;    /* referenced; mapping lives in this VA    */
    PMDL          Mdl;
    PVOID         UserVa;
    SIZE_T        Length;
    INT32         TlbId;      /* window backing this mapping, -1 for BAR */
} TTWIND_USER_MAPPING, *PTTWIND_USER_MAPPING;

/*
 * Per-device context.
 *
 * Lifetime: allocated by the framework at EvtDriverDeviceAdd, torn down
 * with the WDFDEVICE. Resource/identity fields are (re)populated in
 * EvtDevicePrepareHardware and are stable while the device is started.
 */
typedef struct _TTWIND_DEVICE_CONTEXT {
    /* PCI identity, read from config space via BUS_INTERFACE_STANDARD. */
    UINT16 VendorId;
    UINT16 DeviceId;
    UINT16 SubsystemVendorId;
    UINT16 SubsystemId;

    /* PCI location (bus/device/function, segment). */
    UINT8  Bus;
    UINT8  Device;
    UINT8  Function;
    UINT16 PciDomain;

    /*
     * Memory BARs as reported by PnP (CmResourceTypeMemory /
     * CmResourceTypeMemoryLarge, translated). Recorded only - nothing is
     * mapped yet. Indexed in resource-list order, which for this device
     * matches BAR order.
     */
    UINT32 BarCount;
    struct {
        PHYSICAL_ADDRESS Phys;
        UINT64           Size;
    } Bars[TTWIND_MAX_BARS];

    /*
     * Kernel UC mapping of the Blackhole TLB configuration registers
     * (BAR0 + TTWIND_BH_TLB_REGS_START, TTWIND_BH_TLB_REGS_LEN bytes).
     * Mapped at PrepareHardware, unmapped at ReleaseHardware; NULL while
     * the device is not started. This is the only device memory the
     * kernel itself writes.
     */
    PUCHAR TlbRegs;

    /*
     * Kernel UC mapping of the reserved kernel TLB window (BAR0 +
     * TTWIND_BH_KERNEL_TLB_START, 2 MiB). All kernel-initiated NOC
     * traffic (ARC mailbox, scratch registers) goes through this window.
     * Mapped at PrepareHardware, unmapped at ReleaseHardware; NULL while
     * the device is not started. Guarded by ArcLock.
     */
    PUCHAR KernelTlb;

    /*
     * ArcLock serializes ALL use of the ARC mailbox and of the kernel
     * TLB window (every kernel NOC access reprograms the window). Taken
     * at PASSIVE_LEVEL only; ordered after nothing (never acquired while
     * holding StateLock and vice versa).
     */
    WDFWAITLOCK ArcLock;

    /*
     * Config-space access to this function via the parent bus driver.
     * Queried (referenced) at PrepareHardware, dereferenced at
     * ReleaseHardware. Valid only while BusIfValid.
     */
    BUS_INTERFACE_STANDARD BusIf;
    BOOLEAN BusIfValid;

    /*
     * Shared mapping/TLB state, guarded by StateLock (all users run at
     * PASSIVE_LEVEL: sequential-queue ioctl handlers, file cleanup, and
     * ReleaseHardware).
     */
    WDFWAITLOCK   StateLock;
    LIST_ENTRY    MappingList;                        /* TTWIND_USER_MAPPING */
    RTL_BITMAP    TlbBitmap;                          /* 2M window allocator */
    ULONG         TlbBitmapBits[(TTWIND_BH_TLB_2M_COUNT + 31) / 32];
    WDFFILEOBJECT TlbOwner[TTWIND_BH_TLB_2M_COUNT];   /* NULL if free        */
} TTWIND_DEVICE_CONTEXT, *PTTWIND_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TTWIND_DEVICE_CONTEXT, TtWindGetDeviceContext)

/* driver.c */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD TtWindEvtDeviceAdd;

/* device.c */
EVT_WDF_DEVICE_PREPARE_HARDWARE TtWindEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TtWindEvtDeviceReleaseHardware;

/* queue.c */
NTSTATUS TtWindQueueInitialize(_In_ WDFDEVICE Device);
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TtWindEvtIoDeviceControl;

/* mapping.c */
NTSTATUS TtWindMappingInitDevice(_In_ WDFDEVICE Device);
EVT_WDF_FILE_CLEANUP TtWindEvtFileCleanup;
VOID TtWindRevokeAllMappings(_In_ WDFDEVICE Device);
NTSTATUS TtWindIoctlMapBar(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                           _Out_ size_t *BytesWritten);
NTSTATUS TtWindIoctlUnmapBar(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
NTSTATUS TtWindCreateUserMapping(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                                 _In_ PHYSICAL_ADDRESS Phys, _In_ SIZE_T Length,
                                 _In_ UINT32 CacheMode, _In_ INT32 TlbId,
                                 _Out_ PVOID *OutUserVa);
BOOLEAN TtWindTlbHasMappings(_In_ PTTWIND_DEVICE_CONTEXT Ctx, _In_ INT32 TlbId);

/* tlb.c */
NTSTATUS TtWindIoctlAllocateTlb(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                                _Out_ size_t *BytesWritten);
NTSTATUS TtWindIoctlFreeTlb(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
NTSTATUS TtWindIoctlConfigureTlb(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
NTSTATUS TtWindIoctlMapTlb(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                           _Out_ size_t *BytesWritten);
VOID TtWindProgramTlb2M(_In_ PTTWIND_DEVICE_CONTEXT Ctx, _In_ UINT32 TlbId,
                        _In_ const TTWIND_NOC_TLB_CONFIG *Cfg);

/* arc.c */
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT TtWindEvtDeviceSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART TtWindEvtDeviceSelfManagedIoRestart;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND TtWindEvtDeviceSelfManagedIoSuspend;
NTSTATUS TtWindArcMsgSendSync(_In_ WDFDEVICE Device,
                              _Inout_ PTTWIND_ARC_MSG Msg);
NTSTATUS TtWindArcPowerUp(_In_ WDFDEVICE Device);
VOID TtWindArcPowerDown(_In_ WDFDEVICE Device);
NTSTATUS TtWindIoctlSmcMsg(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                           _Out_ size_t *BytesWritten);

/* reset.c */
NTSTATUS TtWindIoctlResetDevice(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
