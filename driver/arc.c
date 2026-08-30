/* SPDX-License-Identifier: Apache-2.0 */
/*
 * arc.c - ARC (SMC) firmware messaging and power management for
 * Tenstorrent Blackhole.
 *
 * Protocol (all facts taken from tt-kmd; file:line refer to the
 * reference tree):
 *
 *  - The ARC management processor sits at NOC0 tile (8, 0)
 *    (blackhole.c:60-61 ARC_X/ARC_Y). Its "reset scratch" registers are
 *    at NOC address 0x80030400 + 4*N (blackhole.c:62 RESET_SCRATCH).
 *  - Scratch 2 is the boot status; bit 0 means the firmware is ready
 *    for mailbox messages (blackhole.c:77-78). tt-kmd polls it for up
 *    to 500 ms (blackhole.c:69, 573-584).
 *  - Scratch 11 holds a pointer (an address inside the ARC CSM SRAM,
 *    0x10000000..+512 KiB, telemetry.h:82-83) to the Queue Control
 *    Block: u32[0] = queue base address, u32[1] & 0xFF = number of
 *    entries (blackhole.c:67, 586-596).
 *  - Queue layout (msgqueue.h:17-23, msgqueue.c): a 32-byte header at
 *    the base (request write pointer at +0x00, response read pointer at
 *    +0x04, request read pointer at +0x10, response write pointer at
 *    +0x14), then num_entries 32-byte request slots, then num_entries
 *    32-byte response slots. Pointers run 0..2*num_entries-1 so
 *    occupancy is (wptr - rptr) mod 2n and the slot index is ptr mod n.
 *  - A message is eight u32s: header word (message type in the low
 *    byte), then seven payload words (msgqueue.h:12-15).
 *  - After pushing a request, writing 0 to NOC address 0x800B0000 on
 *    the ARC tile triggers the firmware's queue processor
 *    (blackhole.c:68, 599-604).
 *  - Responses are polled with a 1000 ms timeout, sleeping 100-200 us
 *    between polls (msgqueue.h:18, msgqueue.c:197-210). An all-1s read
 *    of any queue pointer means the device is gone or the NOC path is
 *    hung (msgqueue.c:27-38).
 *
 * ARC XBAR NOC window (why every ARC NOC address carries ArcNocBase):
 * the scratch registers, CSM, and doorbell all live in the ARC tile's
 * XBAR address space (32-bit: CSM at 0x10000000, APB at 0x80030400,
 * doorbell at 0x800B0000). How that space appears in the tile's
 * NOC-side 64-bit address map has two candidates:
 *  - the LOW ALIAS: NOC address == XBAR address. This is what tt-kmd
 *    uses on Blackhole (blackhole.c:62-68) and what tt-umd's Blackhole
 *    NOC fallback uses (blackhole_arc_apb.cpp:45-48 with
 *    ARC_NOC_XBAR_ADDRESS_START = 0x80000000).
 *  - the HIGH WINDOW at 0x8_00000000 + XBAR address. On Wormhole this
 *    is the ONLY way to reach the ARC over the NOC
 *    (tt-umd wormhole_implementation.hpp:292-297, 308-312: NOC access
 *    = ARC_NOC_ADDRESS_START 0x800000000 + XBAR offset; scratch =
 *    0x880030060; exercised by test_cluster_wh.cpp:1060-1084), and
 *    tt-umd declares the same window for Blackhole as
 *    ARC_NOC_TO_ARC_XBAR_MAP_ADDRESS_START = 0x800000000
 *    (blackhole_implementation.hpp:235), though its Blackhole code
 *    paths do not use it.
 * On this machine's card, every read through the low alias returns
 * 0x00000000 (boot status, QCB pointer, arbitrary XBAR addresses) while
 * the same TLB path reads Tensix tiles correctly - the signature of an
 * unmapped region reading as zero, i.e. firmware/silicon here does not
 * expose the low alias. Discovery therefore probes the low alias first
 * (tt-kmd behavior preserved) and falls back to the high window; the
 * base that answers with a live boot status is cached in
 * Ctx->ArcNocBase and applied to ALL ARC accesses uniformly. Probing is
 * reads-only; queue writes happen only through a window that already
 * answered.
 *
 * All kernel NOC access goes through the reserved topmost 2 MiB TLB
 * window (tt-kmd reserves the same one, blackhole.c:43, 702-703),
 * reprogrammed for each access exactly like tt-kmd's
 * bh_configure_kernel_tlb (blackhole.c:238-251). ArcLock serializes the
 * window and the mailbox; everything here runs at PASSIVE_LEVEL and
 * every wait is a bounded KeDelayExecutionThread - the mailbox path can
 * never spin at elevated IRQL or wait unboundedly.
 *
 * Power management (the reason this file exists): with no power
 * management, a NOC access to a powered-down tile (e.g. an ETH tile)
 * hangs the chip's NOC<->PCIe path until reboot. tt-kmd raises the ASIC
 * to the A0 state at probe (ARC_MSG_TYPE_ASIC_STATE0, blackhole.c:70,
 * 732-736) and raises tile power at open by sending the aggregated
 * POWER_SETTING message (message 0x21; header carries a validity byte
 * and the 15-bit power flags, blackhole.c:74, 919-929; aggregation and
 * the legacy-open default flags in chardev.c:626-693, 1075-1077). We
 * mirror that at device start, and send the corresponding power-down
 * (flags = 0, then ASIC_STATE3, blackhole.c:71, 803-825) at stop.
 */

#include "ttwind.h"

/* ARC tile NOC coordinates (NOC0). */
#define TTWIND_ARC_X 8u
#define TTWIND_ARC_Y 0u

/* Registers on the ARC tile (32-bit ARC XBAR addresses). */
#define TTWIND_RESET_SCRATCH(n)         (0x80030400u + ((n) * 4u))
#define TTWIND_ARC_BOOT_STATUS          TTWIND_RESET_SCRATCH(2)
#define TTWIND_ARC_BOOT_STATUS_READY    0x1u
#define TTWIND_ARC_MSG_QCB_PTR          TTWIND_RESET_SCRATCH(11)
#define TTWIND_ARC_MSI_FIFO             0x800B0000u

/* ARC CSM SRAM; the queue and its control block must lie inside it. */
#define TTWIND_ARC_CSM_BASE             0x10000000u
#define TTWIND_ARC_CSM_SIZE             (1u << 19)

/*
 * Candidate NOC window bases for the ARC XBAR (see the file header):
 * the low alias (tt-kmd's addressing) and the Wormhole-style high
 * window (tt-umd blackhole_implementation.hpp:235).
 */
static const UINT64 TtWindArcXbarBases[] = { 0x0ull, 0x800000000ull };
#define TTWIND_ARC_XBAR_BASE_COUNT ARRAYSIZE(TtWindArcXbarBases)
/* The ARC_STATUS report has exactly a low and a high boot-status slot. */
C_ASSERT(ARRAYSIZE(TtWindArcXbarBases) == 2);

/* Queue geometry (msgqueue.h). */
#define TTWIND_ARC_QUEUE_HEADER_SIZE    32u
#define TTWIND_ARC_REQ_WPTR(base)       ((base) + 0x00u)
#define TTWIND_ARC_RES_RPTR(base)       ((base) + 0x04u)
#define TTWIND_ARC_REQ_RPTR(base)       ((base) + 0x10u)
#define TTWIND_ARC_RES_WPTR(base)       ((base) + 0x14u)

/* Timeouts (ms), from tt-kmd. */
#define TTWIND_ARC_READY_TIMEOUT_MS     500u
#define TTWIND_ARC_MSG_TIMEOUT_MS       1000u

/* Message types used by the driver itself (blackhole.c:70-74). */
#define TTWIND_ARC_MSG_ASIC_STATE0      0xA0u
#define TTWIND_ARC_MSG_ASIC_STATE3     0xA3u
#define TTWIND_ARC_MSG_POWER_SETTING    0x21u

/*
 * POWER_SETTING header: type | validity << 8 | power_flags << 16
 * (blackhole_set_power_state, blackhole.c:924). Validity 0x0F = 15
 * valid flags, 0 valid settings; flags 0x7FFE = everything except
 * MAX_AI_CLK, which is what a legacy tt-kmd open contributes
 * (chardev.c:1075-1077 with the aggregation in chardev.c:626-681).
 */
#define TTWIND_ARC_POWER_VALIDITY       0x0Fu
#define TTWIND_ARC_POWER_FLAGS_UP       0x7FFEu
#define TTWIND_ARC_POWER_FLAGS_DOWN     0x0000u

/* Sleep for Microseconds at PASSIVE_LEVEL. */
static VOID
TtWindArcStall(
    _In_ ULONG Microseconds
    )
{
    LARGE_INTEGER interval;

    interval.QuadPart = -((LONGLONG)Microseconds * 10); /* 100 ns units */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

/* Deadline helpers over the monotonic interrupt-time clock (100 ns). */
static UINT64
TtWindArcDeadline(
    _In_ ULONG Milliseconds
    )
{
    return KeQueryInterruptTime() + ((UINT64)Milliseconds * 10000ull);
}

static BOOLEAN
TtWindArcPastDeadline(
    _In_ UINT64 Deadline
    )
{
    return KeQueryInterruptTime() > Deadline;
}

/*
 * Aim the kernel TLB window at the 2 MiB block of (TTWIND_ARC_X,
 * TTWIND_ARC_Y) NOC0 containing Addr and return a pointer to Addr
 * within the window. Caller holds ArcLock and has verified that
 * Ctx->KernelTlb and Ctx->TlbRegs are mapped. Mirrors tt-kmd's
 * bh_configure_kernel_tlb (strict ordering, unicast, NOC0).
 */
static volatile ULONG *
TtWindArcNocPtr(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT64 Addr
    )
{
    TTWIND_NOC_TLB_CONFIG cfg;

    RtlZeroMemory(&cfg, sizeof(cfg));
    cfg.Addr = Addr & ~((UINT64)TTWIND_BH_TLB_2M_SIZE - 1);
    cfg.XEnd = TTWIND_ARC_X;
    cfg.YEnd = TTWIND_ARC_Y;
    cfg.Noc = 0;
    cfg.Ordering = 1; /* strict */

    TtWindProgramTlb2M(Ctx, TTWIND_BH_KERNEL_TLB_INDEX, &cfg);

    return (volatile ULONG *)
        (Ctx->KernelTlb + (Addr & (TTWIND_BH_TLB_2M_SIZE - 1)));
}

/* 32-bit NOC read/write on the ARC tile. Caller holds ArcLock. */
static UINT32
TtWindArcNocRead32(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT64 Addr
    )
{
    return READ_REGISTER_ULONG(TtWindArcNocPtr(Ctx, Addr));
}

static VOID
TtWindArcNocWrite32(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT64 Addr,
    _In_ UINT32 Value
    )
{
    WRITE_REGISTER_ULONG(TtWindArcNocPtr(Ctx, Addr), Value);
}

/*
 * Bounds-checked accesses to the ARC CSM SRAM, mirroring tt-kmd's
 * csm_read32/csm_write32 (blackhole.c:328-344): every firmware-supplied
 * address (queue control block, queue base, ring pointers) is validated
 * against the CSM range before use so a corrupt pointer can never send
 * the kernel's NOC access somewhere unexpected.
 */
/*
 * XBAR-relative accessors: XbarAddr is an address in the ARC XBAR's
 * 32-bit space (scratch/doorbell/CSM); the NOC address is formed by
 * adding the discovered window base. Callers hold ArcLock and run only
 * after (or as part of) discovery, so ArcNocBase is meaningful.
 */
static UINT32
TtWindArcXbarRead32(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT32 XbarAddr
    )
{
    return TtWindArcNocRead32(Ctx, Ctx->ArcNocBase + XbarAddr);
}

static VOID
TtWindArcXbarWrite32(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT32 XbarAddr,
    _In_ UINT32 Value
    )
{
    TtWindArcNocWrite32(Ctx, Ctx->ArcNocBase + XbarAddr, Value);
}

static NTSTATUS
TtWindArcCsmRead32(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT32 Addr,
    _Out_ UINT32 *Value
    )
{
    *Value = 0;
    if (Addr < TTWIND_ARC_CSM_BASE ||
        Addr > TTWIND_ARC_CSM_BASE + TTWIND_ARC_CSM_SIZE - sizeof(UINT32) ||
        (Addr & 3) != 0) {
        return STATUS_INVALID_ADDRESS;
    }
    *Value = TtWindArcXbarRead32(Ctx, Addr);
    return STATUS_SUCCESS;
}

static NTSTATUS
TtWindArcCsmWrite32(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT32 Addr,
    _In_ UINT32 Value
    )
{
    if (Addr < TTWIND_ARC_CSM_BASE ||
        Addr > TTWIND_ARC_CSM_BASE + TTWIND_ARC_CSM_SIZE - sizeof(UINT32) ||
        (Addr & 3) != 0) {
        return STATUS_INVALID_ADDRESS;
    }
    TtWindArcXbarWrite32(Ctx, Addr, Value);
    return STATUS_SUCCESS;
}

/*
 * Discover the firmware message queue. Mirrors
 * blackhole_arc_msg_locate_queue (blackhole.c:566-597) with one
 * addition: the ARC XBAR NOC window base is probed (see the file
 * header). Each candidate window's boot status is read each poll
 * round; the first window returning a live value with the ready bit
 * wins and is cached in Ctx->ArcNocBase for all subsequent traffic.
 * A cached base is re-verified for free, since the winning probe read
 * IS the boot-status check.
 *
 * Report, when non-NULL, receives the raw observations for the
 * ARC_STATUS diagnostic ioctl (boot status per window, QCB pointer,
 * decoded queue); its Stage/LastStatus are filled by the caller.
 *
 * Waits are bounded (500 ms, tt-kmd's ARC_MSG_READY_MS). Caller holds
 * ArcLock.
 */
static NTSTATUS
TtWindArcLocateQueue(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _Out_ UINT32 *QueueBase,
    _Out_ UINT32 *NumEntries,
    _Inout_opt_ TTWIND_ARC_STATUS_OUT *Report
    )
{
    UINT64 deadline = TtWindArcDeadline(TTWIND_ARC_READY_TIMEOUT_MS);
    UINT32 bootStatus[TTWIND_ARC_XBAR_BASE_COUNT] = { 0 };
    BOOLEAN found = FALSE;
    BOOLEAN allDead;
    UINT32 qcbAddr;
    UINT32 base;
    UINT32 queueInfo;
    ULONG i;
    NTSTATUS status;

    *QueueBase = 0;
    *NumEntries = 0;

    for (;;) {
        allDead = TRUE;

        /*
         * Probe both windows each round (two 32-bit reads); which one
         * answers is a fixed property of the silicon/firmware, so a
         * fixed probe order is deterministic and the cache never
         * flip-flops.
         */
        for (i = 0; i < TTWIND_ARC_XBAR_BASE_COUNT; i++) {
            const UINT64 candidate = TtWindArcXbarBases[i];

            bootStatus[i] = TtWindArcNocRead32(
                Ctx, candidate + TTWIND_ARC_BOOT_STATUS);

            if (bootStatus[i] != 0xFFFFFFFFu) {
                allDead = FALSE;
            }
            if (bootStatus[i] != 0xFFFFFFFFu &&
                (bootStatus[i] & TTWIND_ARC_BOOT_STATUS_READY)) {
                if (!Ctx->ArcNocBaseValid || Ctx->ArcNocBase != candidate) {
                    KdPrint(("ttwind: ARC XBAR NOC window base "
                             "0x%I64X (boot status 0x%08X)\n",
                             candidate, bootStatus[i]));
                }
                Ctx->ArcNocBase = candidate;
                Ctx->ArcNocBaseValid = TRUE;
                found = TRUE;
                break;
            }
        }

        if (Report != NULL) {
            Report->BootStatusLow = bootStatus[0];
            Report->BootStatusHigh = bootStatus[1];
        }

        if (found) {
            break;
        }
        if (allDead) {
            /* Every window reads all-1s: NOC hung or device gone. */
            return STATUS_DEVICE_DOES_NOT_EXIST;
        }
        if (TtWindArcPastDeadline(deadline)) {
            KdPrint(("ttwind: ARC not ready for messages (boot status "
                     "low 0x%08X high 0x%08X)\n",
                     bootStatus[0], bootStatus[1]));
            return STATUS_DEVICE_NOT_READY;
        }
        TtWindArcStall(200);
    }

    if (Report != NULL) {
        Report->NocBase = Ctx->ArcNocBase;
    }

    qcbAddr = TtWindArcXbarRead32(Ctx, TTWIND_ARC_MSG_QCB_PTR);
    if (Report != NULL) {
        Report->QcbPtr = qcbAddr;
    }

    status = TtWindArcCsmRead32(Ctx, qcbAddr + 0, &base);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = TtWindArcCsmRead32(Ctx, qcbAddr + 4, &queueInfo);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *QueueBase = base;
    *NumEntries = queueInfo & 0xFFu;

    if (Report != NULL) {
        Report->QueueBase = base;
        Report->NumEntries = *NumEntries;
    }

    /* A zero-length queue would divide by zero in the ring math. */
    if (*NumEntries == 0) {
        return STATUS_NOT_SUPPORTED;
    }

    /*
     * The whole ring (header + requests + responses) must fit in CSM;
     * checking here makes the per-word accesses below unfailable in
     * the bounds sense.
     */
    if (base < TTWIND_ARC_CSM_BASE ||
        (UINT64)base + TTWIND_ARC_QUEUE_HEADER_SIZE +
            (2ull * *NumEntries * sizeof(TTWIND_ARC_MSG)) >
            (UINT64)TTWIND_ARC_CSM_BASE + TTWIND_ARC_CSM_SIZE) {
        return STATUS_INVALID_ADDRESS;
    }

    return STATUS_SUCCESS;
}

/*
 * Try to push a request. STATUS_SUCCESS, STATUS_DEVICE_BUSY when the
 * ring is full, or an error. Mirrors arc_msg_try_push (msgqueue.c:13).
 * Caller holds ArcLock.
 */
static NTSTATUS
TtWindArcTryPush(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ const TTWIND_ARC_MSG *Msg,
    _In_ UINT32 QueueBase,
    _In_ UINT32 NumEntries
    )
{
    UINT32 requestBase = QueueBase + TTWIND_ARC_QUEUE_HEADER_SIZE;
    UINT32 wptr, rptr;
    UINT32 slot, reqOffset;
    UINT32 i;
    NTSTATUS status;

    status = TtWindArcCsmRead32(Ctx, TTWIND_ARC_REQ_WPTR(QueueBase), &wptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (wptr == 0xFFFFFFFFu) {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    status = TtWindArcCsmRead32(Ctx, TTWIND_ARC_REQ_RPTR(QueueBase), &rptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (rptr == 0xFFFFFFFFu) {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    if (((wptr - rptr) % (2 * NumEntries)) >= NumEntries) {
        return STATUS_DEVICE_BUSY; /* ring full */
    }

    slot = wptr % NumEntries;
    reqOffset = slot * sizeof(TTWIND_ARC_MSG);
    for (i = 0; i < 8; i++) {
        UINT32 addr = requestBase + reqOffset + (i * sizeof(UINT32));
        UINT32 value = (i == 0) ? Msg->Header : Msg->Payload[i - 1];

        status = TtWindArcCsmWrite32(Ctx, addr, value);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    wptr = (wptr + 1) % (2 * NumEntries);
    return TtWindArcCsmWrite32(Ctx, TTWIND_ARC_REQ_WPTR(QueueBase), wptr);
}

/*
 * Try to pop a response. STATUS_SUCCESS, STATUS_DEVICE_BUSY when no
 * response is ready, or an error. Mirrors arc_msg_try_pop
 * (msgqueue.c:61). Caller holds ArcLock.
 */
static NTSTATUS
TtWindArcTryPop(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _Out_ PTTWIND_ARC_MSG Msg,
    _In_ UINT32 QueueBase,
    _In_ UINT32 NumEntries
    )
{
    UINT32 responseBase = QueueBase + TTWIND_ARC_QUEUE_HEADER_SIZE +
                          (NumEntries * sizeof(TTWIND_ARC_MSG));
    UINT32 wptr, rptr;
    UINT32 slot, resOffset;
    UINT32 i;
    NTSTATUS status;

    RtlZeroMemory(Msg, sizeof(*Msg));

    status = TtWindArcCsmRead32(Ctx, TTWIND_ARC_RES_RPTR(QueueBase), &rptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (rptr == 0xFFFFFFFFu) {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    status = TtWindArcCsmRead32(Ctx, TTWIND_ARC_RES_WPTR(QueueBase), &wptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (wptr == 0xFFFFFFFFu) {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    if (((wptr - rptr) % (2 * NumEntries)) == 0) {
        return STATUS_DEVICE_BUSY; /* nothing ready */
    }

    slot = rptr % NumEntries;
    resOffset = slot * sizeof(TTWIND_ARC_MSG);

    status = TtWindArcCsmRead32(Ctx, responseBase + resOffset, &Msg->Header);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (i = 0; i < 7; i++) {
        UINT32 addr = responseBase + resOffset + ((i + 1) * sizeof(UINT32));

        status = TtWindArcCsmRead32(Ctx, addr, &Msg->Payload[i]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    rptr = (rptr + 1) % (2 * NumEntries);
    return TtWindArcCsmWrite32(Ctx, TTWIND_ARC_RES_RPTR(QueueBase), rptr);
}

/*
 * TtWindArcMsgSendSync - one synchronous request/response exchange.
 *
 * Mirrors arc_msg_send_sync (msgqueue.c:158): locate the queue, drain
 * any stale responses left by a prior exchange (bounded by the ring
 * size), push, trigger, poll for the response with a hard 1000 ms
 * timeout. On success *Msg holds the raw response; the firmware status
 * in Msg->Header is NOT interpreted here - kernel callers that require
 * success check it themselves, and the SMC_MSG ioctl passes it through
 * to userspace (matching tt-kmd's SMC_MSG POLL semantics, ioctl.h).
 *
 * Runs at PASSIVE_LEVEL. ArcLock serializes the mailbox; on timeout the
 * call fails gracefully - nothing is left in-flight that a later call
 * cannot drain.
 */
NTSTATUS
TtWindArcMsgSendSync(
    _In_ WDFDEVICE Device,
    _Inout_ PTTWIND_ARC_MSG Msg
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    TTWIND_ARC_MSG drain;
    UINT32 queueBase = 0;
    UINT32 numEntries = 0;
    UINT64 deadline;
    UINT32 i;
    NTSTATUS status;

    WdfWaitLockAcquire(ctx->ArcLock, NULL);

    if (ctx->KernelTlb == NULL || ctx->TlbRegs == NULL) {
        status = STATUS_DEVICE_NOT_READY;
        goto out;
    }

    status = TtWindArcLocateQueue(ctx, &queueBase, &numEntries, NULL);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    /*
     * Discard stale responses from a prior failed/timed-out exchange.
     * The ring holds at most numEntries responses and nothing else is
     * producing, so this drain is bounded.
     */
    for (i = 0; i < numEntries; i++) {
        if (TtWindArcTryPop(ctx, &drain, queueBase, numEntries) !=
            STATUS_SUCCESS) {
            break;
        }
    }

    status = TtWindArcTryPush(ctx, Msg, queueBase, numEntries);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    /* Ring the doorbell: the firmware's queue processor runs on this. */
    TtWindArcXbarWrite32(ctx, TTWIND_ARC_MSI_FIFO, 0);

    deadline = TtWindArcDeadline(TTWIND_ARC_MSG_TIMEOUT_MS);
    for (;;) {
        status = TtWindArcTryPop(ctx, Msg, queueBase, numEntries);
        if (status != STATUS_DEVICE_BUSY) {
            break; /* success or hard error */
        }
        if (TtWindArcPastDeadline(deadline)) {
            KdPrint(("ttwind: timeout waiting for ARC response "
                     "(header 0x%08X)\n", Msg->Header));
            status = STATUS_IO_TIMEOUT;
            break;
        }
        TtWindArcStall(200);
    }

out:
    WdfWaitLockRelease(ctx->ArcLock);
    return status;
}

/*
 * Send one kernel-internal message and require firmware success (a zero
 * response header, like arc_msg_send_sync's -EREMOTEIO check,
 * msgqueue.c:212-213).
 */
static NTSTATUS
TtWindArcSendChecked(
    _In_ WDFDEVICE Device,
    _In_ UINT32 Header,
    _In_ const char *What
    )
{
    TTWIND_ARC_MSG msg;
    NTSTATUS status;

    /* What feeds KdPrint only, which compiles out in free builds. */
    UNREFERENCED_PARAMETER(What);

    RtlZeroMemory(&msg, sizeof(msg));
    msg.Header = Header;

    status = TtWindArcMsgSendSync(Device, &msg);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: ARC message %s (0x%08X) failed 0x%08X\n",
                 What, Header, status));
        return status;
    }
    if (msg.Header != 0) {
        KdPrint(("ttwind: ARC message %s (0x%08X): firmware status "
                 "0x%08X\n", What, Header, msg.Header));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return STATUS_SUCCESS;
}

/*
 * TtWindArcPowerUp - what tt-kmd does at probe + first (legacy) open:
 * raise the ASIC to the A0 state (blackhole_init_hardware,
 * blackhole.c:732-736), then request tile power with the aggregated
 * POWER_SETTING message a legacy open would produce (chardev.c:1106,
 * 626-681): all power flags except MAX_AI_CLK. After this, previously
 * sleeping tiles (e.g. ETH) respond to NOC reads instead of hanging
 * the NOC<->PCIe path.
 *
 * tt-kmd also sets a firmware watchdog at probe (SET_WDT_TIMEOUT,
 * blackhole.c:738-742); deliberately NOT sent here - an unfed watchdog
 * auto-resets the chip, and this driver has no feeding mechanism.
 */
NTSTATUS
TtWindArcPowerUp(
    _In_ WDFDEVICE Device
    )
{
    NTSTATUS status;

    status = TtWindArcSendChecked(Device, TTWIND_ARC_MSG_ASIC_STATE0,
                                  "ASIC_STATE0");
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return TtWindArcSendChecked(
        Device,
        TTWIND_ARC_MSG_POWER_SETTING |
            (TTWIND_ARC_POWER_VALIDITY << 8) |
            ((UINT32)TTWIND_ARC_POWER_FLAGS_UP << 16),
        "POWER_SETTING(up)");
}

/*
 * TtWindArcPowerDown - what tt-kmd does at last close + remove: drop
 * all power flags (the empty aggregation, chardev.c:1153-1179), then
 * put the ASIC in the A3 state (blackhole_cleanup_hardware,
 * blackhole.c:803-825). Best-effort by design: the device may already
 * be gone.
 */
VOID
TtWindArcPowerDown(
    _In_ WDFDEVICE Device
    )
{
    (VOID)TtWindArcSendChecked(
        Device,
        TTWIND_ARC_MSG_POWER_SETTING |
            (TTWIND_ARC_POWER_VALIDITY << 8) |
            ((UINT32)TTWIND_ARC_POWER_FLAGS_DOWN << 16),
        "POWER_SETTING(down)");

    (VOID)TtWindArcSendChecked(Device, TTWIND_ARC_MSG_ASIC_STATE3,
                               "ASIC_STATE3");
}

/*
 * Power-up runs from EvtDeviceSelfManagedIoInit (not PrepareHardware):
 * SelfManagedIo callbacks are the KMDF stage that (a) runs strictly
 * after D0Entry with the device fully powered and started, (b) runs at
 * PASSIVE_LEVEL exactly once per start, and (c) has symmetric Suspend/
 * Restart counterparts that bracket every later stop, rebalance, and
 * system sleep - giving the A3/A0 messages a matching hook on each
 * side. PrepareHardware would also work for this interrupt-less driver,
 * but it has no paired "hardware still reachable" stop-side callback
 * (ReleaseHardware can run after the device is already gone), and a
 * failure there would fail device start - whereas firmware messaging
 * failures are deliberately non-fatal (log and continue, like tt-kmd's
 * dev_err in blackhole_init_hardware).
 */
NTSTATUS
TtWindEvtDeviceSelfManagedIoInit(
    _In_ WDFDEVICE Device
    )
{
    NTSTATUS status = TtWindArcPowerUp(Device);

    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: initial power-up failed 0x%08X (continuing; "
                 "sleeping tiles may hang NOC accesses)\n", status));
    } else {
        KdPrint(("ttwind: ARC power-up complete\n"));
    }
    return STATUS_SUCCESS;
}

NTSTATUS
TtWindEvtDeviceSelfManagedIoRestart(
    _In_ WDFDEVICE Device
    )
{
    /* Same as Init: re-raise power after resume/restart. */
    return TtWindEvtDeviceSelfManagedIoInit(Device);
}

NTSTATUS
TtWindEvtDeviceSelfManagedIoSuspend(
    _In_ WDFDEVICE Device
    )
{
    TtWindArcPowerDown(Device);
    return STATUS_SUCCESS;
}

/*
 * Handler for IOCTL_TTWIND_SMC_MSG - one synchronous user-initiated
 * message exchange. The sequential IOCTL queue plus ArcLock give the
 * "one message at a time" semantics for free; the raw response
 * (firmware status included) is passed through uninterpreted, matching
 * tt-kmd's SMC_MSG POLL contract.
 */
NTSTATUS
TtWindIoctlSmcMsg(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    TTWIND_SMC_MSG_INOUT *in;
    TTWIND_SMC_MSG_INOUT *out;
    TTWIND_ARC_MSG msg;
    UINT32 i;
    NTSTATUS status;

    *BytesWritten = 0;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    msg.Header = in->Message[0];
    for (i = 0; i < 7; i++) {
        msg.Payload[i] = in->Message[i + 1];
    }

    status = TtWindArcMsgSendSync(Device, &msg);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(out, sizeof(*out));
    out->Message[0] = msg.Header;
    for (i = 0; i < 7; i++) {
        out->Message[i + 1] = msg.Payload[i];
    }

    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}

/*
 * Handler for IOCTL_TTWIND_ARC_STATUS - one live, bounded discovery
 * probe, reported raw. Reads only; always succeeds once the buffers
 * check out, with the outcome in the payload (see ttwind_ioctl.h).
 */
NTSTATUS
TtWindIoctlArcStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    TTWIND_ARC_STATUS_OUT *out;
    TTWIND_ARC_STATUS_OUT report;
    UINT32 queueBase = 0;
    UINT32 numEntries = 0;
    NTSTATUS status;

    *BytesWritten = 0;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&report, sizeof(report));
    report.NocBase = ~0ull;

    WdfWaitLockAcquire(ctx->ArcLock, NULL);

    if (ctx->KernelTlb == NULL || ctx->TlbRegs == NULL) {
        report.Stage = TTWIND_ARC_STAGE_NOT_STARTED;
        report.LastStatus = (unsigned int)STATUS_DEVICE_NOT_READY;
    } else {
        status = TtWindArcLocateQueue(ctx, &queueBase, &numEntries,
                                      &report);
        report.LastStatus = (unsigned int)status;
        if (NT_SUCCESS(status)) {
            report.Stage = TTWIND_ARC_STAGE_QUEUE_OK;
        } else if (report.NocBase != ~0ull) {
            /* A window answered ready, but the queue didn't check out. */
            report.Stage = TTWIND_ARC_STAGE_BAD_QUEUE;
        } else {
            report.Stage = TTWIND_ARC_STAGE_NO_BOOT_READY;
        }
    }

    WdfWaitLockRelease(ctx->ArcLock);

    *out = report;
    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}
