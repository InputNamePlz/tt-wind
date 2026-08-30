/* SPDX-License-Identifier: Apache-2.0 */
/*
 * device.c - hardware arrival/departure for tt-wind.
 *
 * EvtDevicePrepareHardware records (without mapping) the PCI memory BARs
 * assigned by PnP and reads the device's PCI identity through the bus
 * interface. Mapping the BARs and touching the hardware come with later
 * milestones (BAR mmap, TLB windows).
 */

#include "ttwind.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, TtWindEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, TtWindEvtDeviceReleaseHardware)
#endif

/*
 * WhichSpace value for BUS_INTERFACE_STANDARD.GetBusData selecting PCI
 * configuration space. Documented as PCI_WHICH_SPACE_CONFIG but only
 * defined in legacy miniport headers, so define it here.
 */
#define PCI_WHICH_SPACE_CONFIG 0x0u

/*
 * Length of a CM_PARTIAL_RESOURCE_DESCRIPTOR memory descriptor in bytes,
 * handling the CmResourceTypeMemoryLarge encodings (40/48/64-bit lengths
 * stored shifted in the 32-bit Length field).
 */
static UINT64
TtWindMemoryDescriptorLength(
    _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR *Desc
    )
{
    if (Desc->Type == CmResourceTypeMemory) {
        return Desc->u.Memory.Length;
    }

    /* CmResourceTypeMemoryLarge */
    if (Desc->Flags & CM_RESOURCE_MEMORY_LARGE_40) {
        return ((UINT64)Desc->u.Memory40.Length40) << 8;
    }
    if (Desc->Flags & CM_RESOURCE_MEMORY_LARGE_48) {
        return ((UINT64)Desc->u.Memory48.Length48) << 16;
    }
    if (Desc->Flags & CM_RESOURCE_MEMORY_LARGE_64) {
        return ((UINT64)Desc->u.Memory64.Length64) << 32;
    }

    return 0;
}

/*
 * Read PCI identity (vendor/device/subsystem IDs) from config space via
 * the parent bus driver's BUS_INTERFACE_STANDARD, and the B/D/F location
 * from the PnP device properties.
 */
static NTSTATUS
TtWindReadPciIdentity(
    _In_ WDFDEVICE Device,
    _Inout_ PTTWIND_DEVICE_CONTEXT Ctx
    )
{
    PCI_COMMON_HEADER pciHeader;
    ULONG bytesRead;
    ULONG busNumber = 0;
    ULONG address = 0;
    ULONG resultLength;
    NTSTATUS status;

    PAGED_CODE();

    /*
     * The PCI bus driver implements BUS_INTERFACE_STANDARD; GetBusData/
     * SetBusData access raw config space. The interface is kept
     * (referenced) in the device context for the device's started
     * lifetime: RESET_DEVICE needs config space access long after
     * PrepareHardware. Dereferenced at ReleaseHardware.
     */
    if (!Ctx->BusIfValid) {
        status = WdfFdoQueryForInterface(Device,
                                         &GUID_BUS_INTERFACE_STANDARD,
                                         (PINTERFACE)&Ctx->BusIf,
                                         sizeof(Ctx->BusIf),
                                         1, /* version */
                                         NULL);
        if (!NT_SUCCESS(status)) {
            KdPrint(("ttwind: BUS_INTERFACE_STANDARD query failed 0x%08X\n",
                     status));
            return status;
        }
        Ctx->BusIfValid = TRUE;
    }

    bytesRead = Ctx->BusIf.GetBusData(Ctx->BusIf.Context,
                                      PCI_WHICH_SPACE_CONFIG,
                                      &pciHeader,
                                      0,
                                      sizeof(pciHeader));

    if (bytesRead != sizeof(pciHeader)) {
        KdPrint(("ttwind: short config space read (%lu bytes)\n", bytesRead));
        return STATUS_DEVICE_DATA_ERROR;
    }

    Ctx->VendorId = pciHeader.VendorID;
    Ctx->DeviceId = pciHeader.DeviceID;

    /* Subsystem IDs live in the type 0 header only. */
    if (PCI_CONFIGURATION_TYPE(&pciHeader) == PCI_DEVICE_TYPE) {
        Ctx->SubsystemVendorId = pciHeader.u.type0.SubVendorID;
        Ctx->SubsystemId = pciHeader.u.type0.SubSystemID;
    }

    /*
     * PCI location. DevicePropertyAddress packs device number in the
     * high word and function number in the low word. The PCI segment
     * (domain) is not exposed through these legacy properties; this
     * machine (and the vast majority of client systems) has a single
     * segment 0, so report 0 until multi-segment support is needed.
     */
    status = WdfDeviceQueryProperty(Device,
                                    DevicePropertyBusNumber,
                                    sizeof(busNumber),
                                    &busNumber,
                                    &resultLength);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: bus number query failed 0x%08X\n", status));
        return status;
    }

    status = WdfDeviceQueryProperty(Device,
                                    DevicePropertyAddress,
                                    sizeof(address),
                                    &address,
                                    &resultLength);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: device address query failed 0x%08X\n", status));
        return status;
    }

    Ctx->Bus = (UINT8)busNumber;
    Ctx->Device = (UINT8)((address >> 16) & 0xFFFF);
    Ctx->Function = (UINT8)(address & 0xFFFF);
    Ctx->PciDomain = 0;

    return STATUS_SUCCESS;
}

/*
 * TtWindEvtDevicePrepareHardware - record the translated memory
 * resources (the PCIe BARs) and read the PCI identity.
 *
 * Nothing is mapped here yet: milestone 1 only needs to know where the
 * BARs are. MmMapIoSpaceEx of the TLB/register regions arrives with the
 * BAR-mapping milestone.
 */
NTSTATUS
TtWindEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    ULONG count;
    ULONG i;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    PAGED_CODE();

    status = TtWindReadPciIdentity(Device, ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KdPrint(("ttwind: device %04X:%04X (subsystem %04X:%04X) at "
             "%04X:%02X:%02X.%X\n",
             ctx->VendorId, ctx->DeviceId,
             ctx->SubsystemVendorId, ctx->SubsystemId,
             ctx->PciDomain, ctx->Bus, ctx->Device, ctx->Function));

    /*
     * Walk the translated resources and record every memory descriptor.
     * The PCI bus driver hands the BARs over in BAR order; interrupt and
     * other resource types are skipped for now.
     */
    ctx->BarCount = 0;
    RtlZeroMemory(ctx->Bars, sizeof(ctx->Bars));

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i++) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (desc->Type != CmResourceTypeMemory &&
            desc->Type != CmResourceTypeMemoryLarge) {
            continue;
        }

        if (ctx->BarCount >= TTWIND_MAX_BARS) {
            KdPrint(("ttwind: more than %u memory resources; ignoring "
                     "extras\n", TTWIND_MAX_BARS));
            break;
        }

        ctx->Bars[ctx->BarCount].Phys = desc->u.Memory.Start;
        ctx->Bars[ctx->BarCount].Size = TtWindMemoryDescriptorLength(desc);

        KdPrint(("ttwind: BAR[%u] phys 0x%I64X size 0x%I64X\n",
                 ctx->BarCount,
                 (UINT64)ctx->Bars[ctx->BarCount].Phys.QuadPart,
                 ctx->Bars[ctx->BarCount].Size));

        ctx->BarCount++;
    }

    /*
     * Map the Blackhole TLB configuration registers (a 4 KiB block high
     * in BAR0) for kernel use - the only device memory the kernel
     * itself touches, and only from CONFIGURE_TLB. A device whose BAR0
     * cannot hold them is not a device this driver can operate.
     */
    if (ctx->BarCount == 0 ||
        ctx->Bars[0].Size <
            (UINT64)TTWIND_BH_TLB_REGS_START + TTWIND_BH_TLB_REGS_LEN) {
        KdPrint(("ttwind: BAR0 missing or too small for TLB registers\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    {
        PHYSICAL_ADDRESS regsPhys;

        regsPhys.QuadPart = ctx->Bars[0].Phys.QuadPart +
                            TTWIND_BH_TLB_REGS_START;
        ctx->TlbRegs = (PUCHAR)MmMapIoSpaceEx(regsPhys,
                                              TTWIND_BH_TLB_REGS_LEN,
                                              PAGE_READWRITE | PAGE_NOCACHE);
        if (ctx->TlbRegs == NULL) {
            KdPrint(("ttwind: MmMapIoSpaceEx(TLB regs) failed\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /*
     * Map the reserved kernel TLB window (topmost 2 MiB window of BAR0)
     * for kernel-initiated NOC access - the ARC firmware mailbox and
     * scratch registers (arc.c). Its start lies below the TLB register
     * block, so the BAR0 size check above already covers it.
     */
    {
        PHYSICAL_ADDRESS winPhys;

        winPhys.QuadPart = ctx->Bars[0].Phys.QuadPart +
                           (LONGLONG)TTWIND_BH_KERNEL_TLB_START;
        ctx->KernelTlb = (PUCHAR)MmMapIoSpaceEx(winPhys,
                                                TTWIND_BH_TLB_2M_SIZE,
                                                PAGE_READWRITE | PAGE_NOCACHE);
        if (ctx->KernelTlb == NULL) {
            KdPrint(("ttwind: MmMapIoSpaceEx(kernel TLB window) failed\n"));
            MmUnmapIoSpace(ctx->TlbRegs, TTWIND_BH_TLB_REGS_LEN);
            ctx->TlbRegs = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /*
     * Map the ARC APB AXI aperture (BAR0 + 0x1FF00000, 1 MiB) - one of
     * the candidate routes to the ARC scratch registers (arc.c).
     * Best-effort: without it the NOC routes are still probed.
     */
    if (ctx->Bars[0].Size >=
        (UINT64)TTWIND_BH_ARC_APB_BAR0_START + TTWIND_BH_ARC_APB_BAR0_LEN) {
        PHYSICAL_ADDRESS apbPhys;

        apbPhys.QuadPart = ctx->Bars[0].Phys.QuadPart +
                           TTWIND_BH_ARC_APB_BAR0_START;
        ctx->ArcApbAxi = (PUCHAR)MmMapIoSpaceEx(apbPhys,
                                                TTWIND_BH_ARC_APB_BAR0_LEN,
                                                PAGE_READWRITE | PAGE_NOCACHE);
        if (ctx->ArcApbAxi == NULL) {
            KdPrint(("ttwind: MmMapIoSpaceEx(ARC APB aperture) failed; "
                     "continuing with NOC routes only\n"));
        }
    }

    return STATUS_SUCCESS;
}

/*
 * TtWindEvtDeviceReleaseHardware - undo PrepareHardware.
 *
 * Force-unmap every user mapping first (the BAR physical ranges are
 * being taken away, so no user view may survive), then drop the kernel
 * TLB register mapping and forget the recorded resources; identity is
 * left in place for late queries during removal.
 */
NTSTATUS
TtWindEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    KdPrint(("ttwind: release hardware\n"));

    TtWindRevokeAllMappings(Device);

    /*
     * The ARC mailbox path checks KernelTlb/TlbRegs under ArcLock; the
     * SelfManagedIoSuspend power-down and queue purge have already
     * happened, so nothing can be mid-exchange, but take the lock so
     * the pointers never change under a reader.
     */
    WdfWaitLockAcquire(ctx->ArcLock, NULL);
    if (ctx->KernelTlb != NULL) {
        MmUnmapIoSpace(ctx->KernelTlb, TTWIND_BH_TLB_2M_SIZE);
        ctx->KernelTlb = NULL;
    }
    if (ctx->ArcApbAxi != NULL) {
        MmUnmapIoSpace(ctx->ArcApbAxi, TTWIND_BH_ARC_APB_BAR0_LEN);
        ctx->ArcApbAxi = NULL;
    }
    ctx->ArcRoute = TTWIND_ARC_ROUTE_NONE;
    WdfWaitLockRelease(ctx->ArcLock);

    /*
     * The power-managed default queue is already purged here, so no
     * CONFIGURE_TLB can be in flight; take the lock anyway so TlbRegs
     * never changes under a reader.
     */
    WdfWaitLockAcquire(ctx->StateLock, NULL);
    if (ctx->TlbRegs != NULL) {
        MmUnmapIoSpace(ctx->TlbRegs, TTWIND_BH_TLB_REGS_LEN);
        ctx->TlbRegs = NULL;
    }
    WdfWaitLockRelease(ctx->StateLock);

    if (ctx->BusIfValid) {
        ctx->BusIf.InterfaceDereference(ctx->BusIf.Context);
        ctx->BusIfValid = FALSE;
    }

    ctx->BarCount = 0;
    RtlZeroMemory(ctx->Bars, sizeof(ctx->Bars));

    return STATUS_SUCCESS;
}
