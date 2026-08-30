/* SPDX-License-Identifier: Apache-2.0 */
/*
 * driver.c - DriverEntry and device creation for tt-wind.
 *
 * tt-wind is a KMDF PnP function driver for Tenstorrent PCIe accelerators
 * (Blackhole for now). This file wires up the framework driver object and
 * creates the per-device WDFDEVICE with its interface and I/O queue.
 */

#include <initguid.h> /* instantiate GUID_DEVINTERFACE_TTWIND here */
#include "ttwind.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, TtWindEvtDeviceAdd)
#endif

/*
 * DriverEntry - framework driver object creation.
 *
 * No unload routine is supplied; WDF handles unload for a driver that
 * has nothing to tear down beyond its framework objects.
 */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    KdPrint(("ttwind: DriverEntry\n"));

    WDF_DRIVER_CONFIG_INIT(&config, TtWindEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: WdfDriverCreate failed 0x%08X\n", status));
    }

    return status;
}

/*
 * TtWindEvtDeviceAdd - called by the framework for each matching PCI
 * function. Creates the WDFDEVICE, its context, the default I/O queue,
 * and registers the device interface user mode enumerates by.
 */
NTSTATUS
TtWindEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    /*
     * Hardware arrival/departure. Prepare/Release record the PCI BARs
     * and device identity; D0 entry/exit callbacks come later, when
     * there is hardware state to manage.
     */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = TtWindEvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = TtWindEvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    /*
     * IOCTL buffers are copied by the I/O manager (METHOD_BUFFERED), and
     * nothing here touches user addresses directly, so buffered I/O is
     * the right default.
     */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    /*
     * Per-handle cleanup: every user mapping and TLB window is owned by
     * the file object that created it, and EvtFileCleanup tears down
     * whatever the closing handle still holds. No create/close handlers
     * are needed - the framework's defaults complete those.
     */
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               WDF_NO_EVENT_CALLBACK, /* create  */
                               WDF_NO_EVENT_CALLBACK, /* close   */
                               TtWindEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit,
                                     &fileConfig,
                                     WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, TTWIND_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    /*
     * Context starts zeroed by the framework; set up the mapping/TLB
     * bookkeeping (lists, allocator bitmap, state lock).
     */
    status = TtWindMappingInitDevice(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = TtWindQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Register the device interface. Each Tenstorrent function gets one
     * instance; user mode lists them with CM_Get_Device_Interface_List
     * and opens the symbolic links with CreateFile.
     */
    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_TTWIND,
                                            NULL /* no reference string */);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: WdfDeviceCreateDeviceInterface failed 0x%08X\n",
                 status));
        return status;
    }

    return STATUS_SUCCESS;
}
