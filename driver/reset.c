/* SPDX-License-Identifier: Apache-2.0 */
/*
 * reset.c - IOCTL_TTWIND_RESET_DEVICE for Tenstorrent Blackhole.
 *
 * Reset mechanism: the device's OWN config space, not the parent bridge
 * ------------------------------------------------------------------
 * tt-kmd's standard Blackhole reset (TENSTORRENT_RESET_DEVICE_ASIC_RESET,
 * chardev.c:322-328) is pcie_timer_interrupt (pcie.c:133-138): two
 * writes to the DEVICE'S own config space - DBI "interface timer"
 * registers at 0x934 (target = 1) and 0x930 (enable | force pending) -
 * which make the chip reset itself. Afterwards tt-kmd restores the
 * endpoint's saved config space and re-runs the hardware init messages
 * (POST_RESET, chardev.c:336-354).
 *
 * That mechanism was chosen here over the alternatives:
 *  - Secondary-bus hot reset (tt-kmd pcie.c:61-90) needs the PARENT
 *    bridge's Bridge Control register. A KMDF function driver has no
 *    supported way to reach the parent's config space; hacking there
 *    (HalGetBusData on the parent's bus/slot, or a private devnode
 *    walk) risks fighting the PCI driver's ownership of the bridge, and
 *    a wrong bus reset can hang the root port. Rejected.
 *  - FLR: not used anywhere by tt-kmd for these devices, so it is not
 *    the vendor-validated path even if advertised. Rejected.
 *  - ARC TRIGGER_RESET message (tt-kmd's ASIC_DMC_RESET): requires the
 *    firmware mailbox to be alive, but the main reason to reset is a
 *    wedged NOC/firmware. The config-write mechanism works regardless.
 * The chosen path touches only this function's config space, which the
 * PCI driver tolerates from its FDO, and never the link or the bridge.
 *
 * Conservative v1 semantics: refused with STATUS_DEVICE_BUSY while ANY
 * user mapping exists (there is no VMA-zap equivalent implemented on
 * Windows yet; dangling user views of BAR space across a reset are
 * unacceptable). New mappings cannot appear during the reset because
 * the ioctl runs on the driver's sequential queue.
 *
 * Known risks (documented, accepted for a dev-machine v1):
 *  - Only the first 256 bytes of config space are saved/restored (the
 *    standard header plus the capability chain where Blackhole keeps
 *    MSI/PCIe caps). Extended (>0x100) capabilities revert to reset
 *    defaults.
 *  - If the platform reports the momentary link-down as a surprise
 *    removal, PnP may tear the device down mid-reset; tt-kmd suppresses
 *    hotplug for the bridge, we cannot. On the target machine the link
 *    stays up through this reset (it is the mechanism tt-smi uses).
 */

#include "ttwind.h"

/* WhichSpace selector for PCI config space (see device.c). */
#define TTWIND_PCI_WHICH_SPACE_CONFIG 0x0u

/* Standard config-space offsets/bits used below. */
#define TTWIND_PCI_CFG_VENDOR_ID  0x00u
#define TTWIND_PCI_CFG_COMMAND    0x04u
#define TTWIND_PCI_CMD_PARITY     0x0040u  /* PCI_ENABLE_PARITY */

/* How much config space to save/restore (header + capability chain). */
#define TTWIND_PCI_CFG_SAVE_BYTES 256u

/*
 * Blackhole DBI interface-timer registers, exposed in config space
 * (tt-kmd pcie.c:17-22, pcie_timer_interrupt pcie.c:133-138).
 */
#define TTWIND_BH_IFC_TIMER_CONTROL_OFF 0x930u
#define TTWIND_BH_IFC_TIMER_TARGET_OFF  0x934u
#define TTWIND_BH_IFC_TIMER_TARGET      0x1u
#define TTWIND_BH_IFC_TIMER_EN          0x1u
#define TTWIND_BH_IFC_FORCE_PENDING     0x10u

/* Bounded waits. */
#define TTWIND_RESET_LINK_TIMEOUT_MS  10000u /* tt-kmd poll_pcie_link_up */
#define TTWIND_RESET_LINK_POLL_MS     100u
#define TTWIND_RESET_SETTLE_MS        500u
#define TTWIND_RESET_POWERUP_TRIES    5u
#define TTWIND_RESET_POWERUP_RETRY_MS 1000u

static VOID
TtWindResetStallMs(
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER interval;

    interval.QuadPart = -((LONGLONG)Milliseconds * 10000); /* 100 ns */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

/* Config space accessors over the cached BUS_INTERFACE_STANDARD. */
static BOOLEAN
TtWindCfgRead(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    return Ctx->BusIf.GetBusData(Ctx->BusIf.Context,
                                 TTWIND_PCI_WHICH_SPACE_CONFIG,
                                 Buffer, Offset, Length) == Length;
}

static BOOLEAN
TtWindCfgWrite(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    return Ctx->BusIf.SetBusData(Ctx->BusIf.Context,
                                 TTWIND_PCI_WHICH_SPACE_CONFIG,
                                 Buffer, Offset, Length) == Length;
}

/*
 * Poll (bounded) until config space reads back our vendor ID, i.e. the
 * device has come out of reset and responds to config cycles. Mirrors
 * tt-kmd's poll_pcie_link_up (pcie.c:24-41).
 */
static BOOLEAN
TtWindResetWaitForDevice(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx
    )
{
    ULONG waited = 0;

    for (;;) {
        UINT16 vendor = 0xFFFF;

        if (TtWindCfgRead(Ctx, TTWIND_PCI_CFG_VENDOR_ID, &vendor,
                          sizeof(vendor)) &&
            vendor == Ctx->VendorId) {
            return TRUE;
        }

        if (waited >= TTWIND_RESET_LINK_TIMEOUT_MS) {
            return FALSE;
        }
        TtWindResetStallMs(TTWIND_RESET_LINK_POLL_MS);
        waited += TTWIND_RESET_LINK_POLL_MS;
    }
}

/*
 * Restore the saved 256 bytes of config space, command register last so
 * memory/IO decode and bus mastering come back only after the BARs and
 * capabilities hold their pre-reset values. Read-only registers ignore
 * the writes; status-register RW1C bits at worst clear stale status.
 */
static BOOLEAN
TtWindResetRestoreConfig(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_reads_(TTWIND_PCI_CFG_SAVE_BYTES / 4) const UINT32 *Saved
    )
{
    BOOLEAN ok = TRUE;
    ULONG i;

    for (i = 0; i < TTWIND_PCI_CFG_SAVE_BYTES / 4; i++) {
        UINT32 dword = Saved[i];

        if (i == TTWIND_PCI_CFG_COMMAND / 4) {
            continue; /* command/status last */
        }
        if (!TtWindCfgWrite(Ctx, i * 4, &dword, sizeof(dword))) {
            ok = FALSE;
        }
    }

    {
        UINT32 cmdStatus = Saved[TTWIND_PCI_CFG_COMMAND / 4];

        if (!TtWindCfgWrite(Ctx, TTWIND_PCI_CFG_COMMAND, &cmdStatus,
                            sizeof(cmdStatus))) {
            ok = FALSE;
        }
    }

    return ok;
}

/*
 * Handler for IOCTL_TTWIND_RESET_DEVICE.
 */
NTSTATUS
TtWindIoctlResetDevice(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    TTWIND_RESET_DEVICE_IN *in;
    UINT32 savedConfig[TTWIND_PCI_CFG_SAVE_BYTES / 4];
    UINT16 command;
    UINT32 dword;
    BOOLEAN busy;
    ULONG attempt;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in->Flags != 0 || in->Reserved != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!ctx->BusIfValid || ctx->BarCount == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    /*
     * Conservative v1: refuse while any user mapping exists, on any
     * handle. The sequential queue guarantees no MAP ioctl can create
     * a new one while this handler runs (file cleanup only removes).
     */
    WdfWaitLockAcquire(ctx->StateLock, NULL);
    busy = !IsListEmpty(&ctx->MappingList);
    WdfWaitLockRelease(ctx->StateLock);
    if (busy) {
        return STATUS_DEVICE_BUSY;
    }

    /*
     * Hold ArcLock across the whole reset so no kernel mailbox user
     * (e.g. a racing SelfManagedIo transition) touches the device while
     * it is down.
     */
    WdfWaitLockAcquire(ctx->ArcLock, NULL);

    /* 1. Save config space (before the reset marker dirties COMMAND). */
    if (!TtWindCfgRead(ctx, 0, savedConfig, sizeof(savedConfig))) {
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }
    if ((savedConfig[0] & 0xFFFF) != ctx->VendorId) {
        /* Device already unreachable; nothing sane to restore later. */
        status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto out;
    }

    /*
     * 2. Reset marker (tt-kmd set_reset_marker, pcie.c:140-149): set
     * the COMMAND parity-error-response bit; a real reset clears it.
     */
    command = (UINT16)(savedConfig[TTWIND_PCI_CFG_COMMAND / 4] & 0xFFFF);
    command |= TTWIND_PCI_CMD_PARITY;
    if (!TtWindCfgWrite(ctx, TTWIND_PCI_CFG_COMMAND, &command,
                        sizeof(command))) {
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }

    /* 3. Trigger: the interface-timer config writes (pcie.c:133-138). */
    dword = TTWIND_BH_IFC_TIMER_TARGET;
    (VOID)TtWindCfgWrite(ctx, TTWIND_BH_IFC_TIMER_TARGET_OFF, &dword,
                         sizeof(dword));
    dword = TTWIND_BH_IFC_TIMER_EN | TTWIND_BH_IFC_FORCE_PENDING;
    (VOID)TtWindCfgWrite(ctx, TTWIND_BH_IFC_TIMER_CONTROL_OFF, &dword,
                         sizeof(dword));

    /* 4. Let the reset take, then wait for config space to respond. */
    TtWindResetStallMs(TTWIND_RESET_SETTLE_MS);
    if (!TtWindResetWaitForDevice(ctx)) {
        KdPrint(("ttwind: device did not return after reset\n"));
        status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto out;
    }

    /*
     * 5. Verify the marker cleared, i.e. the chip actually reset. If it
     * is still set the trigger was ignored; config space is untouched,
     * so just clear our marker and report failure.
     */
    if (!TtWindCfgRead(ctx, TTWIND_PCI_CFG_COMMAND, &command,
                       sizeof(command))) {
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }
    if (command & TTWIND_PCI_CMD_PARITY) {
        KdPrint(("ttwind: reset marker still set; reset did not occur\n"));
        command = (UINT16)(savedConfig[TTWIND_PCI_CFG_COMMAND / 4] & 0xFFFF);
        (VOID)TtWindCfgWrite(ctx, TTWIND_PCI_CFG_COMMAND, &command,
                             sizeof(command));
        status = STATUS_UNSUCCESSFUL;
        goto out;
    }

    /* 6. Restore config space (BARs, MSI, PCIe DevCtl incl. MPS/MRRS). */
    if (!TtWindResetRestoreConfig(ctx, savedConfig)) {
        KdPrint(("ttwind: config space restore incomplete\n"));
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }

    KdPrint(("ttwind: device reset complete; re-initializing firmware "
             "state\n"));
    status = STATUS_SUCCESS;

out:
    WdfWaitLockRelease(ctx->ArcLock);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * 7. PrepareHardware-equivalent re-init. The kernel mappings
     * (TlbRegs, KernelTlb) map BAR0, whose address was just restored,
     * so they are valid again; the TLB window registers themselves
     * reset, but every kernel NOC access reprograms the kernel window
     * and user windows hold no mappings (checked above). What remains
     * is the firmware power-up, retried while the freshly rebooted ARC
     * comes up (each attempt already waits up to 500 ms for the boot
     * status ready bit; total bound ~10 s).
     */
    for (attempt = 1; ; attempt++) {
        status = TtWindArcPowerUp(Device);
        if (NT_SUCCESS(status)) {
            break;
        }
        if (attempt >= TTWIND_RESET_POWERUP_TRIES) {
            KdPrint(("ttwind: post-reset power-up failed 0x%08X\n",
                     status));
            return STATUS_DEVICE_HARDWARE_ERROR;
        }
        TtWindResetStallMs(TTWIND_RESET_POWERUP_RETRY_MS);
    }

    return STATUS_SUCCESS;
}
