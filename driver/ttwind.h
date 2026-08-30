/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ttwind.h - private declarations of the tt-wind KMDF driver.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdmguid.h> /* GUID_BUS_INTERFACE_STANDARD */

#include "ttwind_ioctl.h"

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
