/* SPDX-License-Identifier: Apache-2.0 */
/*
 * reset.c - IOCTL_TTWIND_RESET_DEVICE / IOCTL_TTWIND_POST_RESET for
 * Tenstorrent Blackhole.
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
 * Split arm / recover model (v2) - incident 2026-08-30
 * ----------------------------------------------------
 * On 2026-08-30 the previous single-ioctl reset was issued against a
 * wedged chip (all MMIO reads 0xFFFFFFFF with ~200 ms completion-
 * timeout latency, config space still readable). The ioctl returned a
 * failure status, and moments later the machine hard-froze
 * (Kernel-Power 41, no minidump). Analysis: the DBI reset-timer arm
 * write had landed, the chip dropped off the bus, and a subsequent
 * driver-side MMIO (BAR-mapped) read hit the link-down window. THIS
 * PLATFORM STALLS SUCH READS instead of synthesizing all-1s: the CPU
 * sticks at an uninterruptible MMIO access and the machine freezes
 * without a bugcheck. Config-space accesses (BUS_INTERFACE_STANDARD
 * GetBusData/SetBusData) stay safe - config reads to a missing device
 * return all-1s from the root complex.
 *
 * The fix mirrors tt-kmd's production flow: the reset ioctl ARMS and
 * RETURNS - no sleep, no poll, and above all no MMIO after the arm
 * writes. Recovery is the separate POST_RESET ioctl, which user mode
 * polls (bounded, ~10 s at 100 ms) until the device answers config
 * cycles again. Between arm and successful recovery the device context
 * is in the RESTRICTED state (Ctx->NeedsHwInit): the dispatcher
 * (queue.c) allows only GET_DEVICE_INFO / RESET_DEVICE / POST_RESET,
 * and the ARC mailbox path (arc.c, which also serves the SelfManagedIo
 * power callbacks) refuses under ArcLock - so NOTHING in the driver can
 * touch MMIO while the link may be down. The state is always
 * recoverable (POST_RESET is retryable; a failed recovery leaves it
 * restricted, never permanently dead - only PnP removal is dead).
 *
 * Deliberately NOT probed before arming: any MMIO/ARC liveness. The
 * reset must be available precisely when MMIO is wedged, so triage is
 * config-space-only (vendor ID all-1s => device already off the bus =>
 * refuse; nothing we arm via config space can reach it). tt-kmd
 * likewise probes nothing before a plain ASIC reset.
 *
 * Reset generation: each armed reset bumps Ctx->ResetGeneration (every
 * other open handle becomes stale and gets STATUS_DEVICE_REMOVED from
 * the dispatcher; the resetting handle is carried forward), and all
 * TLB window allocations are reclaimed - stale handles must not hold
 * hardware resources across a reset, and their EvtFileCleanup finding
 * nothing later is harmless.
 *
 * Residual risk that CANNOT be fixed here: a USER-mode mapping of BAR
 * space would stall the same way if it read during link-down. That is
 * why RESET_DEVICE still refuses (STATUS_DEVICE_BUSY) while any user
 * mapping exists, and why MAP_BAR/MAP_TLB are blocked while
 * restricted. A user mapping that exists when the link drops for some
 * OTHER reason (not this ioctl) remains exposed to the platform's
 * MMIO-stall behavior; that residual risk is documented here and
 * accepted.
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

/*
 * Blackhole DBI interface-timer registers, exposed in config space
 * (tt-kmd pcie.c:17-22, pcie_timer_interrupt pcie.c:133-138).
 */
#define TTWIND_BH_IFC_TIMER_CONTROL_OFF 0x930u
#define TTWIND_BH_IFC_TIMER_TARGET_OFF  0x934u
#define TTWIND_BH_IFC_TIMER_TARGET      0x1u
#define TTWIND_BH_IFC_TIMER_EN          0x1u
#define TTWIND_BH_IFC_FORCE_PENDING     0x10u

/* Bounded ARC power-up retry (POST_RESET only; the device is up). */
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

/* One live config-space vendor-ID probe. TRUE when the device answers. */
static BOOLEAN
TtWindDevicePresent(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx
    )
{
    UINT16 vendor = 0xFFFF;

    return TtWindCfgRead(Ctx, TTWIND_PCI_CFG_VENDOR_ID, &vendor,
                         sizeof(vendor)) &&
           vendor == Ctx->VendorId;
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
 * Handler for IOCTL_TTWIND_RESET_DEVICE - ARM ONLY.
 *
 * Sequence (config space only; no sleep, no poll, NO MMIO anywhere):
 *
 *   checks     input, device started, no user mappings, live vendor ID
 *      |
 *   fence      bump reset generation (stale-ify other handles; carry
 *      |       this one forward), reclaim all TLB allocations, save
 *      |       config space (skipped on a re-arm - the original save
 *      |       is the good one), set the reset marker
 *      |
 *   RESTRICT   NeedsHwInit = TRUE (dispatcher + ARC path now refuse
 *      |       everything MMIO-shaped BEFORE touching hardware)
 *      |
 *   ARM        DBI interface-timer config writes; return SUCCESS
 *
 * Ordering matters: the RESTRICT flag goes up BEFORE the arm writes,
 * so from the instant the chip can drop off the bus, no driver path
 * will issue an MMIO access (see the file header for the incident this
 * prevents). ArcLock is held so an in-flight SelfManagedIo power
 * transition finishes its mailbox traffic before we arm.
 */
NTSTATUS
TtWindIoctlResetDevice(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    TTWIND_RESET_DEVICE_IN *in;
    UINT16 command;
    UINT32 dword;
    BOOLEAN busy;
    ULONG i;
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
     * Conservative v1 rule, kept: refuse while any user mapping exists,
     * on any handle. The sequential queue guarantees no MAP ioctl can
     * create a new one while this handler runs (file cleanup only
     * removes). This is machine-safety, not just API hygiene: a user
     * read of BAR space during the link-down window stalls the CPU
     * exactly like a kernel one (file header), and the driver cannot
     * revoke user PTEs on Windows yet.
     */
    WdfWaitLockAcquire(ctx->StateLock, NULL);
    busy = !IsListEmpty(&ctx->MappingList);
    WdfWaitLockRelease(ctx->StateLock);
    if (busy) {
        return STATUS_DEVICE_BUSY;
    }

    /*
     * ArcLock: wait out any in-flight kernel mailbox use (a racing
     * SelfManagedIo transition - ioctls are behind us on the sequential
     * queue) so its MMIO completes while the device is still up, and
     * serialize NeedsHwInit/SavedConfig against arc.c's readers.
     */
    WdfWaitLockAcquire(ctx->ArcLock, NULL);

    /*
     * Config-space-ONLY triage. Deliberately no MMIO/ARC liveness probe
     * of any kind (see file header): a wedged-MMIO chip with live
     * config space is exactly what this reset exists to recover.
     */
    if (!TtWindDevicePresent(ctx)) {
        /*
         * Vendor ID all-1s/wrong: the device is already off the bus.
         * Nothing we arm via its config space can reach it - refuse
         * with a distinct status and change no state (if a previous
         * reset is mid-flight, POST_RESET polling remains the path).
         */
        status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto out;
    }

    /*
     * Save config space - only when not already restricted: on a
     * re-arm the live config is post-reset junk and the ORIGINAL save
     * is the one worth restoring (back-to-back resets idempotent).
     */
    if (!ctx->NeedsHwInit) {
        if (!TtWindCfgRead(ctx, 0, ctx->SavedConfig,
                           sizeof(ctx->SavedConfig))) {
            status = STATUS_DEVICE_DATA_ERROR;
            goto out;
        }
        ctx->SavedConfigValid = TRUE;
    }
    NT_ASSERT(ctx->SavedConfigValid);

    /*
     * Reset marker (tt-kmd set_reset_marker, pcie.c:140-149): set the
     * COMMAND parity-error-response bit on the LIVE value; a real reset
     * clears it, and POST_RESET reads it to tell "chip reset" from
     * "chip ignored the trigger". Nothing may touch the COMMAND
     * register between here and that check.
     */
    if (!TtWindCfgRead(ctx, TTWIND_PCI_CFG_COMMAND, &command,
                       sizeof(command))) {
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }
    command |= TTWIND_PCI_CMD_PARITY;
    if (!TtWindCfgWrite(ctx, TTWIND_PCI_CFG_COMMAND, &command,
                        sizeof(command))) {
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }

    /*
     * Generation fence: every OTHER open handle becomes stale (their
     * ioctls fail with STATUS_DEVICE_REMOVED at dispatch, queue.c);
     * this handle carries forward. Reclaim every TLB window allocation
     * now - stale handles must not hold hardware resources across the
     * reset, and we verified above that none of them is user-mapped.
     * Their later EvtFileCleanup finding nothing is harmless.
     */
    WdfWaitLockAcquire(ctx->StateLock, NULL);
    NT_ASSERT(IsListEmpty(&ctx->MappingList));
    ctx->ResetGeneration++;
    for (i = 0; i < TTWIND_BH_TLB_2M_COUNT; i++) {
        ctx->TlbOwner[i] = NULL;
    }
    RtlClearAllBits(&ctx->TlbBitmap);
    RtlSetBit(&ctx->TlbBitmap, TTWIND_BH_KERNEL_TLB_INDEX);
    WdfWaitLockRelease(ctx->StateLock);

    if (fileObject != NULL) {
        TtWindGetFileContext(fileObject)->ResetGeneration =
            ctx->ResetGeneration;
    }

    /*
     * RESTRICT before ARM: from the instant the trigger below can take
     * effect, no driver path may issue an MMIO access (file header).
     */
    ctx->NeedsHwInit = TRUE;

    /*
     * ARM: the interface-timer config writes (pcie.c:133-138). Written
     * blind like tt-kmd does - there is no meaningful failure check,
     * and POST_RESET's marker test detects a trigger that never took.
     * NOTHING may follow these writes except the return: no sleep, no
     * poll, no MMIO - the chip will drop off the bus any moment now.
     */
    dword = TTWIND_BH_IFC_TIMER_TARGET;
    (VOID)TtWindCfgWrite(ctx, TTWIND_BH_IFC_TIMER_TARGET_OFF, &dword,
                         sizeof(dword));
    dword = TTWIND_BH_IFC_TIMER_EN | TTWIND_BH_IFC_FORCE_PENDING;
    (VOID)TtWindCfgWrite(ctx, TTWIND_BH_IFC_TIMER_CONTROL_OFF, &dword,
                         sizeof(dword));

    KdPrint(("ttwind: reset armed (generation %I64u); poll POST_RESET\n",
             ctx->ResetGeneration));
    status = STATUS_SUCCESS;

out:
    WdfWaitLockRelease(ctx->ArcLock);
    return status;
}

/*
 * Handler for IOCTL_TTWIND_POST_RESET - the recovery half.
 *
 * Cheap, idempotent, retryable; user mode polls it (~100 ms period)
 * after RESET_DEVICE until it stops answering "not back yet".
 *
 *   PROBE      config vendor ID; not back -> DOES_NOT_EXIST (retry)
 *      |
 *   MARKER     COMMAND parity bit still set -> the DBI timer has not
 *      |       fired yet (or never will): stay restricted, return
 *      |       DEVICE_BUSY ("reset pending" - retry). The caller owns
 *      |       the "ignored the trigger" diagnosis, made only after
 *      |       its whole budget expires with the marker still set.
 *      |
 *   RESTORE    full config restore (BARs, MSI, PCIe DevCtl incl.
 *      |       MaxPayload/MaxReadRequest - the MaxReadRequest re-init
 *      |       comes with the restore; COMMAND last), then re-save the
 *      |       restored config for the next reset
 *      |          \- failure -> DATA_ERROR, still restricted (retry)
 *      |
 *   MMIO GATE  ONE bounded BAR0 register write+readback
 *      |       (TtWindKernelTlbSanityCheck) - the FIRST MMIO touch
 *      |       since the arm; the link is confirmed up at the config
 *      |       level so the read completes (worst case all-1s), and
 *      |       an unusable link fails cleanly here instead of being
 *      |       discovered by the ARC path mid-message.
 *      |          \- failure -> IO_DEVICE_ERROR, still restricted
 *      |
 *   UNRESTRICT NeedsHwInit = FALSE
 *      |
 *   RE-INIT    ARC firmware power-up, retried while the freshly
 *              rebooted ARC boots (bounded ~10 s); BEST-EFFORT - a
 *              failure is logged, not returned (tt-kmd logs and
 *              continues likewise; diagnose via ARC_STATUS).
 */
NTSTATUS
TtWindIoctlPostReset(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    TTWIND_POST_RESET_IN *in;
    UINT16 command;
    BOOLEAN recovered = FALSE;
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

    WdfWaitLockAcquire(ctx->ArcLock, NULL);

    if (!ctx->NeedsHwInit) {
        /* Nothing pending (or already recovered): idempotent success. */
        status = STATUS_SUCCESS;
        goto out;
    }

    if (ctx->TlbRegs == NULL || ctx->KernelTlb == NULL ||
        !ctx->SavedConfigValid) {
        status = STATUS_DEVICE_NOT_READY;
        goto out;
    }

    /* PROBE: is the device back on the bus? Config space only. */
    if (!TtWindDevicePresent(ctx)) {
        status = STATUS_DEVICE_DOES_NOT_EXIST; /* keep polling */
        goto out;
    }

    /*
     * MARKER: untouched since the arm (nothing writes COMMAND between
     * RESET_DEVICE's marker write and this read). Still set means the
     * DBI timer has not fired YET - or the chip is ignoring the
     * trigger; the two are indistinguishable from one sample, so stay
     * restricted and report "reset pending" (retryable). The terminal
     * "ignored the trigger" diagnosis belongs to the CALLER, after its
     * whole polling budget expires with the marker never clearing.
     *
     * v100.3.4 got this wrong: it treated the first marker-still-set
     * poll (~100 ms after arm) as terminal, un-restricted the device,
     * and the timer then fired moments later - leaving a freshly reset
     * chip unrestored and unguarded. tt-kmd's userspace sleeps ~2 s
     * before even starting to poll (warm_reset.cpp); our grace period
     * lives in the caller too, but the driver must stay safe against
     * any polling cadence.
     */
    if (!TtWindCfgRead(ctx, TTWIND_PCI_CFG_COMMAND, &command,
                       sizeof(command))) {
        status = STATUS_DEVICE_DOES_NOT_EXIST; /* flaky; keep polling */
        goto out;
    }
    if (command & TTWIND_PCI_CMD_PARITY) {
        status = STATUS_DEVICE_BUSY; /* reset pending; keep polling */
        goto out;
    }

    /* RESTORE (BARs, MSI, PCIe DevCtl incl. MPS/MRRS; COMMAND last). */
    if (!TtWindResetRestoreConfig(ctx, ctx->SavedConfig)) {
        KdPrint(("ttwind: config space restore incomplete; still "
                 "restricted - retry POST_RESET\n"));
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }

    /*
     * Re-save the restored state so a future reset restores from the
     * freshest known-good image (tt-kmd re-saves after restore too).
     * Best-effort: on a short read keep the existing (equal) save.
     */
    if (!TtWindCfgRead(ctx, 0, ctx->SavedConfig,
                       sizeof(ctx->SavedConfig))) {
        KdPrint(("ttwind: post-restore config re-save failed; keeping "
                 "previous save\n"));
    }

    /*
     * MMIO GATE: the first MMIO touch since the arm - one bounded BAR0
     * TLB-register write+readback. Only after this passes is any other
     * MMIO (kernel TLB reprogramming, ARC traffic) allowed again.
     */
    status = TtWindKernelTlbSanityCheck(ctx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: post-reset MMIO sanity readback failed; still "
                 "restricted - retry POST_RESET or re-arm RESET_DEVICE\n"));
        status = STATUS_IO_DEVICE_ERROR;
        goto out;
    }

    /* UNRESTRICT: MMIO proved out end to end. */
    ctx->NeedsHwInit = FALSE;
    recovered = TRUE;
    KdPrint(("ttwind: device reset recovery complete; re-initializing "
             "firmware state\n"));
    status = STATUS_SUCCESS;

out:
    WdfWaitLockRelease(ctx->ArcLock);

    if (!recovered) {
        return status;
    }

    /*
     * RE-INIT: firmware power-up, retried while the freshly rebooted
     * ARC comes up (each attempt already waits up to 500 ms for the
     * boot-status ready bit; total bound ~10 s). BEST-EFFORT and
     * deliberately non-fatal, like tt-kmd's dev_err-and-continue: the
     * link and MMIO are proven alive, so a silent ARC is a firmware
     * matter - diagnose via ARC_STATUS, poke via SMC_MSG. The kernel
     * mappings (TlbRegs, KernelTlb) map BAR0, whose address was just
     * restored, so they are valid again; the TLB window registers
     * themselves reset, but every kernel NOC access reprograms the
     * kernel window and user windows hold no mappings (reclaimed at
     * arm, mappings refused since).
     */
    for (attempt = 1; ; attempt++) {
        status = TtWindArcPowerUp(Device);
        if (NT_SUCCESS(status)) {
            break;
        }
        if (attempt >= TTWIND_RESET_POWERUP_TRIES) {
            KdPrint(("ttwind: post-reset ARC power-up failed 0x%08X "
                     "(continuing; see ARC_STATUS)\n", status));
            break;
        }
        TtWindResetStallMs(TTWIND_RESET_POWERUP_RETRY_MS);
    }

    return STATUS_SUCCESS;
}
