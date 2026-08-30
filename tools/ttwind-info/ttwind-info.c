/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ttwind-info.c - user-mode test CLI for the tt-wind driver.
 *
 * With no arguments: enumerates device interfaces of
 * GUID_DEVINTERFACE_TTWIND, opens each one, issues
 * IOCTL_TTWIND_GET_DEVICE_INFO, and pretty-prints the result. Exits 0
 * if at least one device was queried successfully, 1 if no devices are
 * present, 2 on error.
 *
 * Subcommands (all operate on the first enumerated device):
 *   bar0read <offset>      MAP_BAR(bar0, page around offset, UC), read
 *                          a u32, print it, unmap.
 *   tlbread <x> <y> <addr> Allocate a 2 MiB TLB window, configure it
 *                          for a NOC0 unicast to (x, y) at addr's 2 MiB
 *                          block, MAP_TLB UC, read a u32 at addr within
 *                          the window, print it, unmap, free.
 *   arcmsg <hdr> [w1..w7]  Send a raw 8x u32 ARC (SMC) firmware message
 *                          (missing words are 0), print the response.
 *   arcstatus              Run one ARC discovery probe and print the raw
 *                          observations (boot status via all candidate
 *                          routes, selected route, QCB pointer, queue
 *                          geometry).
 *   sysmem                 QUERY_SYSMEM: print the host sysmem buffer's
 *                          size/NOC address/PCIe tile (or "unavailable").
 *   sysmemtest             MAP_SYSMEM the first 1 MiB, write a pattern
 *                          at offset 0x100 through the cached view, read
 *                          it back over the NOC via a TLB window at the
 *                          PCIe tile (4<<58 + 0x100), PASS/FAIL.
 *   reset                  IOCTL_TTWIND_RESET_DEVICE (arm; refused while
 *                          any user mapping exists) then poll
 *                          IOCTL_TTWIND_POST_RESET until recovery.
 * Numeric arguments accept 0x-prefixed hex.
 */

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../driver/ttwind_ioctl.h"

/* Print "<prefix>: <err> (<message>)" using FormatMessage. */
static void print_win32_error(const char *prefix, DWORD err)
{
    char msg[512];
    DWORD n;

    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       msg, sizeof(msg), NULL);
    if (n != 0) {
        /* Strip trailing CR/LF that FormatMessage appends. */
        while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n'))
            msg[--n] = '\0';
        fprintf(stderr, "%s: error %lu (%s)\n", prefix, err, msg);
    } else {
        fprintf(stderr, "%s: error %lu\n", prefix, err);
    }
}

/* Format a byte count into "16.0 MiB (0x1000000)" style. buf must be
 * at least 64 chars. */
static const char *format_size(unsigned __int64 bytes, char *buf, size_t buflen)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    size_t u = 0;

    while (v >= 1024.0 && u + 1 < ARRAYSIZE(units)) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, buflen, "%llu %s", bytes, units[u]);
    else
        snprintf(buf, buflen, "%.1f %s (0x%llx)", v, units[u], bytes);
    return buf;
}

/* Open one interface path and print its device info. Returns 0 on
 * success, nonzero on failure. */
static int query_device(const wchar_t *path, unsigned index)
{
    TTWIND_DEVICE_INFO_OUT info;
    HANDLE h;
    DWORD returned = 0;
    char sizebuf[64];
    unsigned i;

    printf("Device %u: %ls\n", index, path);

    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        print_win32_error("  CreateFile failed", GetLastError());
        return 1;
    }

    if (!DeviceIoControl(h, IOCTL_TTWIND_GET_DEVICE_INFO, NULL, 0,
                         &info, sizeof(info), &returned, NULL)) {
        print_win32_error("  DeviceIoControl(IOCTL_TTWIND_GET_DEVICE_INFO) failed",
                          GetLastError());
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);

    if (returned < sizeof(info)) {
        fprintf(stderr, "  short IOCTL reply: %lu bytes, expected %zu\n",
                returned, sizeof(info));
        return 1;
    }

    printf("  Vendor ID     : 0x%04x\n", info.VendorId);
    printf("  Device ID     : 0x%04x\n", info.DeviceId);
    printf("  Subsystem     : 0x%04x:0x%04x\n",
           info.SubsystemVendorId, info.SubsystemId);
    printf("  PCI location  : %04x:%02x:%02x.%x\n",
           info.PciDomain, info.Bus, info.Device, info.Function);
    printf("  BAR count     : %u\n", info.BarCount);
    for (i = 0; i < TTWIND_MAX_BARS; i++) {
        if (info.Bars[i].Phys == 0 && info.Bars[i].Size == 0)
            continue;
        printf("  BAR%u          : phys 0x%016llx  size %s\n", i,
               info.Bars[i].Phys,
               format_size(info.Bars[i].Size, sizebuf, sizeof(sizebuf)));
    }
    return 0;
}

/* Return the interface path list (caller frees) or NULL on error. */
static wchar_t *get_interface_list(void)
{
    wchar_t *list = NULL;
    ULONG chars = 0;
    CONFIGRET cr;

    /* The list size can change between the two calls if devices arrive;
     * retry in that case. */
    for (;;) {
        cr = CM_Get_Device_Interface_List_SizeW(
                &chars, (LPGUID)&GUID_DEVINTERFACE_TTWIND, NULL,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS) {
            fprintf(stderr, "CM_Get_Device_Interface_List_Size failed: CR 0x%x\n", cr);
            return NULL;
        }

        free(list);
        list = calloc(chars, sizeof(wchar_t));
        if (list == NULL) {
            fprintf(stderr, "out of memory\n");
            return NULL;
        }

        cr = CM_Get_Device_Interface_ListW(
                (LPGUID)&GUID_DEVINTERFACE_TTWIND, NULL, list, chars,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS)
            return list;
        if (cr != CR_BUFFER_SMALL) {
            fprintf(stderr, "CM_Get_Device_Interface_List failed: CR 0x%x\n", cr);
            free(list);
            return NULL;
        }
    }
}

/* Open the first present tt-wind device, or INVALID_HANDLE_VALUE. */
static HANDLE open_first_device(void)
{
    wchar_t *list = get_interface_list();
    HANDLE h = INVALID_HANDLE_VALUE;

    if (list == NULL)
        return INVALID_HANDLE_VALUE;
    if (*list == L'\0') {
        fprintf(stderr, "No tt-wind devices found.\n");
        free(list);
        return INVALID_HANDLE_VALUE;
    }

    printf("Using device: %ls\n", list);
    h = CreateFileW(list, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        print_win32_error("CreateFile failed", GetLastError());
    free(list);
    return h;
}

/* strtoull with 0x support and error checking; returns 0 on success. */
static int parse_u64(const char *s, unsigned __int64 *out)
{
    char *end = NULL;

    *out = _strtoui64(s, &end, 0);
    if (end == s || *end != '\0') {
        fprintf(stderr, "bad number: '%s'\n", s);
        return 1;
    }
    return 0;
}

/*
 * bar0read <offset>: map the 4 KiB page of BAR0 containing offset (UC),
 * read the u32 at offset, print, unmap.
 */
static int cmd_bar0read(const char *offset_arg)
{
    TTWIND_MAP_BAR_IN map_in;
    TTWIND_MAP_BAR_OUT map_out;
    TTWIND_UNMAP_BAR_IN unmap_in;
    unsigned __int64 offset;
    volatile unsigned int *reg;
    unsigned int value;
    DWORD returned = 0;
    HANDLE h;
    int rc = 2;

    if (parse_u64(offset_arg, &offset))
        return 2;
    if (offset % 4 != 0) {
        fprintf(stderr, "offset must be 4-byte aligned\n");
        return 2;
    }

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    memset(&map_in, 0, sizeof(map_in));
    map_in.BarIndex = 0;
    map_in.CacheMode = TTWIND_CACHE_UC;
    map_in.Offset = offset & ~0xFFFull;
    map_in.Length = 0x1000;

    if (!DeviceIoControl(h, IOCTL_TTWIND_MAP_BAR, &map_in, sizeof(map_in),
                         &map_out, sizeof(map_out), &returned, NULL)) {
        print_win32_error("MAP_BAR failed", GetLastError());
        goto out_close;
    }

    reg = (volatile unsigned int *)(ULONG_PTR)
          (map_out.UserVa + (offset & 0xFFFull));
    value = *reg;
    printf("BAR0[0x%llx] = 0x%08x\n", offset, value);
    rc = 0;

    memset(&unmap_in, 0, sizeof(unmap_in));
    unmap_in.UserVa = map_out.UserVa;
    if (!DeviceIoControl(h, IOCTL_TTWIND_UNMAP_BAR, &unmap_in,
                         sizeof(unmap_in), NULL, 0, &returned, NULL)) {
        print_win32_error("UNMAP_BAR failed", GetLastError());
        rc = 2;
    }

out_close:
    CloseHandle(h);
    return rc;
}

/*
 * tlbread <x> <y> <addr>: allocate a 2 MiB TLB window, aim it at core
 * (x, y) on NOC0 (unicast, strict ordering) covering addr's 2 MiB
 * block, map it UC, read the u32 at addr, print, unmap, free.
 */
static int cmd_tlbread(const char *x_arg, const char *y_arg,
                       const char *addr_arg)
{
    static const unsigned __int64 win_mask = TTWIND_TLB_WINDOW_SIZE_2M - 1;
    TTWIND_ALLOCATE_TLB_IN alloc_in;
    TTWIND_ALLOCATE_TLB_OUT alloc_out;
    TTWIND_CONFIGURE_TLB_IN cfg_in;
    TTWIND_MAP_TLB_IN map_in;
    TTWIND_MAP_TLB_OUT map_out;
    TTWIND_UNMAP_BAR_IN unmap_in;
    TTWIND_FREE_TLB_IN free_in;
    unsigned __int64 x, y, addr;
    volatile unsigned int *reg;
    unsigned int value;
    DWORD returned = 0;
    HANDLE h;
    int rc = 2;

    if (parse_u64(x_arg, &x) || parse_u64(y_arg, &y) ||
        parse_u64(addr_arg, &addr))
        return 2;
    if (x > 0x3F || y > 0x3F) {
        fprintf(stderr, "coordinates must be 0..63\n");
        return 2;
    }
    if (addr % 4 != 0) {
        fprintf(stderr, "addr must be 4-byte aligned\n");
        return 2;
    }

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    memset(&alloc_in, 0, sizeof(alloc_in));
    alloc_in.Size = TTWIND_TLB_WINDOW_SIZE_2M;
    if (!DeviceIoControl(h, IOCTL_TTWIND_ALLOCATE_TLB, &alloc_in,
                         sizeof(alloc_in), &alloc_out, sizeof(alloc_out),
                         &returned, NULL)) {
        print_win32_error("ALLOCATE_TLB failed", GetLastError());
        goto out_close;
    }
    printf("Allocated TLB window %u\n", alloc_out.TlbId);

    memset(&cfg_in, 0, sizeof(cfg_in));
    cfg_in.TlbId = alloc_out.TlbId;
    cfg_in.Config.Addr = addr & ~win_mask;
    cfg_in.Config.XEnd = (unsigned short)x;
    cfg_in.Config.YEnd = (unsigned short)y;
    cfg_in.Config.Noc = 0;
    cfg_in.Config.Ordering = 1; /* strict, like tt-kmd's kernel TLB */
    if (!DeviceIoControl(h, IOCTL_TTWIND_CONFIGURE_TLB, &cfg_in,
                         sizeof(cfg_in), NULL, 0, &returned, NULL)) {
        print_win32_error("CONFIGURE_TLB failed", GetLastError());
        goto out_free;
    }

    memset(&map_in, 0, sizeof(map_in));
    map_in.TlbId = alloc_out.TlbId;
    map_in.CacheMode = TTWIND_CACHE_UC;
    if (!DeviceIoControl(h, IOCTL_TTWIND_MAP_TLB, &map_in, sizeof(map_in),
                         &map_out, sizeof(map_out), &returned, NULL)) {
        print_win32_error("MAP_TLB failed", GetLastError());
        goto out_free;
    }

    reg = (volatile unsigned int *)(ULONG_PTR)
          (map_out.UserVa + (addr & win_mask));
    value = *reg;
    printf("NOC0 (%llu, %llu) [0x%llx] = 0x%08x\n", x, y, addr, value);
    rc = 0;

    memset(&unmap_in, 0, sizeof(unmap_in));
    unmap_in.UserVa = map_out.UserVa;
    if (!DeviceIoControl(h, IOCTL_TTWIND_UNMAP_BAR, &unmap_in,
                         sizeof(unmap_in), NULL, 0, &returned, NULL)) {
        print_win32_error("UNMAP_BAR failed", GetLastError());
        rc = 2;
    }

out_free:
    memset(&free_in, 0, sizeof(free_in));
    free_in.TlbId = alloc_out.TlbId;
    if (!DeviceIoControl(h, IOCTL_TTWIND_FREE_TLB, &free_in,
                         sizeof(free_in), NULL, 0, &returned, NULL)) {
        print_win32_error("FREE_TLB failed", GetLastError());
        rc = 2;
    }
out_close:
    CloseHandle(h);
    return rc;
}

/*
 * tlbreadx: like tlbread, but every CONFIGURE_TLB field is settable via
 * key=value arguments so window configurations can be A/B tested from
 * user mode without touching the driver. Defaults reproduce tlbread's
 * configuration (NOC0 unicast, strict ordering, UC mapping).
 *
 *   ttwind-info tlbreadx addr=<addr> xend=<x> yend=<y> [xstart=] [ystart=]
 *               [noc=] [mcast=] [ordering=] [linked=] [staticvc=] [wc=0|1]
 */
static int cmd_tlbreadx(int argc, char **argv)
{
    static const unsigned __int64 win_mask = TTWIND_TLB_WINDOW_SIZE_2M - 1;
    TTWIND_ALLOCATE_TLB_IN alloc_in;
    TTWIND_ALLOCATE_TLB_OUT alloc_out;
    TTWIND_CONFIGURE_TLB_IN cfg_in;
    TTWIND_MAP_TLB_IN map_in;
    TTWIND_MAP_TLB_OUT map_out;
    TTWIND_UNMAP_BAR_IN unmap_in;
    TTWIND_FREE_TLB_IN free_in;
    unsigned __int64 addr = 0, xend = 0, yend = 0, xstart = 0, ystart = 0;
    unsigned __int64 noc = 0, mcast = 0, ordering = 1, linked = 0;
    unsigned __int64 staticvc = 0, wc = 0;
    int have_addr = 0;
    volatile unsigned int *reg;
    unsigned int value;
    DWORD returned = 0;
    HANDLE h;
    int i;
    int rc = 2;

    for (i = 0; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        unsigned __int64 *dst = NULL;

        if (eq == NULL) {
            fprintf(stderr, "expected key=value, got '%s'\n", argv[i]);
            return 2;
        }
        if (strncmp(argv[i], "addr=", 5) == 0) {
            dst = &addr;
            have_addr = 1;
        } else if (strncmp(argv[i], "xend=", 5) == 0) {
            dst = &xend;
        } else if (strncmp(argv[i], "yend=", 5) == 0) {
            dst = &yend;
        } else if (strncmp(argv[i], "xstart=", 7) == 0) {
            dst = &xstart;
        } else if (strncmp(argv[i], "ystart=", 7) == 0) {
            dst = &ystart;
        } else if (strncmp(argv[i], "noc=", 4) == 0) {
            dst = &noc;
        } else if (strncmp(argv[i], "mcast=", 6) == 0) {
            dst = &mcast;
        } else if (strncmp(argv[i], "ordering=", 9) == 0) {
            dst = &ordering;
        } else if (strncmp(argv[i], "linked=", 7) == 0) {
            dst = &linked;
        } else if (strncmp(argv[i], "staticvc=", 9) == 0) {
            dst = &staticvc;
        } else if (strncmp(argv[i], "wc=", 3) == 0) {
            dst = &wc;
        } else {
            fprintf(stderr, "unknown key in '%s'\n", argv[i]);
            return 2;
        }
        if (parse_u64(eq + 1, dst))
            return 2;
    }

    if (!have_addr) {
        fprintf(stderr, "tlbreadx: addr= is required\n");
        return 2;
    }
    if (addr % 4 != 0) {
        fprintf(stderr, "addr must be 4-byte aligned\n");
        return 2;
    }
    if (xend > 0x3F || yend > 0x3F || xstart > 0x3F || ystart > 0x3F ||
        noc > 1 || mcast > 1 || ordering > 3 || linked > 1 ||
        staticvc > 1 || wc > 1) {
        fprintf(stderr, "field out of range\n");
        return 2;
    }

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    memset(&alloc_in, 0, sizeof(alloc_in));
    alloc_in.Size = TTWIND_TLB_WINDOW_SIZE_2M;
    if (!DeviceIoControl(h, IOCTL_TTWIND_ALLOCATE_TLB, &alloc_in,
                         sizeof(alloc_in), &alloc_out, sizeof(alloc_out),
                         &returned, NULL)) {
        print_win32_error("ALLOCATE_TLB failed", GetLastError());
        goto out_close;
    }

    memset(&cfg_in, 0, sizeof(cfg_in));
    cfg_in.TlbId = alloc_out.TlbId;
    cfg_in.Config.Addr = addr & ~win_mask;
    cfg_in.Config.XEnd = (unsigned short)xend;
    cfg_in.Config.YEnd = (unsigned short)yend;
    cfg_in.Config.XStart = (unsigned short)xstart;
    cfg_in.Config.YStart = (unsigned short)ystart;
    cfg_in.Config.Noc = (unsigned char)noc;
    cfg_in.Config.Mcast = (unsigned char)mcast;
    cfg_in.Config.Ordering = (unsigned char)ordering;
    cfg_in.Config.Linked = (unsigned char)linked;
    cfg_in.Config.StaticVc = (unsigned char)staticvc;

    printf("TLB %u <- addr=0x%llx end=(%llu,%llu) start=(%llu,%llu) "
           "noc=%llu mcast=%llu ordering=%llu linked=%llu staticvc=%llu "
           "cache=%s\n",
           alloc_out.TlbId, cfg_in.Config.Addr, xend, yend, xstart, ystart,
           noc, mcast, ordering, linked, staticvc, wc ? "WC" : "UC");

    if (!DeviceIoControl(h, IOCTL_TTWIND_CONFIGURE_TLB, &cfg_in,
                         sizeof(cfg_in), NULL, 0, &returned, NULL)) {
        print_win32_error("CONFIGURE_TLB failed", GetLastError());
        goto out_free;
    }

    memset(&map_in, 0, sizeof(map_in));
    map_in.TlbId = alloc_out.TlbId;
    map_in.CacheMode = wc ? TTWIND_CACHE_WC : TTWIND_CACHE_UC;
    if (!DeviceIoControl(h, IOCTL_TTWIND_MAP_TLB, &map_in, sizeof(map_in),
                         &map_out, sizeof(map_out), &returned, NULL)) {
        print_win32_error("MAP_TLB failed", GetLastError());
        goto out_free;
    }

    reg = (volatile unsigned int *)(ULONG_PTR)
          (map_out.UserVa + (addr & win_mask));
    value = *reg;
    printf("NOC%llu (%llu, %llu) [0x%llx] = 0x%08x\n",
           noc, xend, yend, addr, value);
    rc = 0;

    memset(&unmap_in, 0, sizeof(unmap_in));
    unmap_in.UserVa = map_out.UserVa;
    if (!DeviceIoControl(h, IOCTL_TTWIND_UNMAP_BAR, &unmap_in,
                         sizeof(unmap_in), NULL, 0, &returned, NULL)) {
        print_win32_error("UNMAP_BAR failed", GetLastError());
        rc = 2;
    }

out_free:
    memset(&free_in, 0, sizeof(free_in));
    free_in.TlbId = alloc_out.TlbId;
    if (!DeviceIoControl(h, IOCTL_TTWIND_FREE_TLB, &free_in,
                         sizeof(free_in), NULL, 0, &returned, NULL)) {
        print_win32_error("FREE_TLB failed", GetLastError());
        rc = 2;
    }
out_close:
    CloseHandle(h);
    return rc;
}

/*
 * arcmsg <hdr> [w1..w7]: one synchronous ARC (SMC) firmware message.
 * Word 0 is the header (message type in the low byte); unspecified
 * words are zero. Prints the raw 8-word response; the firmware status
 * is response word 0 (0 = success).
 *
 * A safe read-only smoke test is the firmware's TEST message, type
 * 0x90 (tt-kmd uses it as a pure liveness check): `arcmsg 0x90`
 * should return status 0.
 */
static int cmd_arcmsg(int argc, char **argv)
{
    TTWIND_SMC_MSG_INOUT msg_in;
    TTWIND_SMC_MSG_INOUT msg_out;
    DWORD returned = 0;
    HANDLE h;
    int i;
    int rc = 2;

    memset(&msg_in, 0, sizeof(msg_in));
    for (i = 0; i < argc; i++) {
        unsigned __int64 word;

        if (parse_u64(argv[i], &word))
            return 2;
        if (word > 0xFFFFFFFFull) {
            fprintf(stderr, "word %d ('%s') does not fit in 32 bits\n",
                    i, argv[i]);
            return 2;
        }
        msg_in.Message[i] = (unsigned int)word;
    }

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    printf("request : ");
    for (i = 0; i < 8; i++)
        printf("%08x%s", msg_in.Message[i], i == 7 ? "\n" : " ");

    if (!DeviceIoControl(h, IOCTL_TTWIND_SMC_MSG, &msg_in, sizeof(msg_in),
                         &msg_out, sizeof(msg_out), &returned, NULL)) {
        print_win32_error("SMC_MSG failed", GetLastError());
        goto out_close;
    }
    if (returned < sizeof(msg_out)) {
        fprintf(stderr, "short SMC_MSG reply: %lu bytes\n", returned);
        goto out_close;
    }

    printf("response: ");
    for (i = 0; i < 8; i++)
        printf("%08x%s", msg_out.Message[i], i == 7 ? "\n" : " ");
    printf("firmware status: 0x%08x (%s)\n", msg_out.Message[0],
           msg_out.Message[0] == 0 ? "ok" : "error");
    rc = 0;

out_close:
    CloseHandle(h);
    return rc;
}

/*
 * arcstatus: IOCTL_TTWIND_ARC_STATUS - one live, bounded discovery
 * probe, printed raw. Safe: reads only.
 */
static int cmd_arcstatus(void)
{
    static const char *const stage_names[] = {
        "0 (device not started)",
        "1 (no route showed boot-ready)",
        "2 (ready, but queue control block/queue invalid)",
        "3 (message queue located)",
    };
    static const char *const route_names[] = {
        "none",
        "NOC low alias (0x80000000 + APB offset)",
        "NOC high window (0x8_80000000 + APB offset)",
        "BAR0 AXI aperture (0x1FF00000 + APB offset)",
    };
    TTWIND_ARC_STATUS_OUT st;
    DWORD returned = 0;
    HANDLE h;
    int rc = 2;

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    if (!DeviceIoControl(h, IOCTL_TTWIND_ARC_STATUS, NULL, 0,
                         &st, sizeof(st), &returned, NULL)) {
        print_win32_error("ARC_STATUS failed", GetLastError());
        goto out_close;
    }
    if (returned < sizeof(st)) {
        fprintf(stderr, "short ARC_STATUS reply: %lu bytes\n", returned);
        goto out_close;
    }

    printf("Stage           : %s\n",
           st.Stage < 4 ? stage_names[st.Stage] : "?");
    printf("Probe NTSTATUS  : 0x%08x\n", st.LastStatus);
    printf("Boot status     : noc-low=0x%08x  noc-high=0x%08x  "
           "axi=0x%08x\n",
           st.BootStatusLow, st.BootStatusHigh, st.BootStatusAxi);
    printf("Selected route  : %s\n",
           st.Route < 4 ? route_names[st.Route] : "?");
    printf("QCB pointer     : 0x%08x\n", st.QcbPtr);
    printf("Queue           : base 0x%08x, %u entries\n",
           st.QueueBase, st.NumEntries);
    rc = (st.Stage == TTWIND_ARC_STAGE_QUEUE_OK) ? 0 : 1;

out_close:
    CloseHandle(h);
    return rc;
}

/*
 * reset: the split arm/recover flow (driver 100.3.4.0+). RESET_DEVICE
 * only ARMS the chip's self-reset and returns immediately (the driver
 * never sleeps or touches MMIO while the device may be off the bus -
 * the 2026-08-30 hard-freeze fix); this tool then polls POST_RESET
 * (config-space probe + config restore + MMIO gate + firmware
 * power-up) about every 100 ms until it succeeds, ~15 s bound.
 * RESET_DEVICE is refused with ERROR_BUSY while any user mapping of
 * device memory exists.
 */
static int cmd_reset(void)
{
    TTWIND_RESET_DEVICE_IN reset_in;
    TTWIND_POST_RESET_IN post_in;
    DWORD returned = 0;
    DWORD err;
    DWORD waited;
    HANDLE h;
    int rc = 2;

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    memset(&reset_in, 0, sizeof(reset_in));
    printf("Arming device reset...\n");
    if (!DeviceIoControl(h, IOCTL_TTWIND_RESET_DEVICE, &reset_in,
                         sizeof(reset_in), NULL, 0, &returned, NULL)) {
        print_win32_error("RESET_DEVICE failed", GetLastError());
        goto out_close;
    }

    /* Grace period before the first poll: the DBI timer takes a moment
     * to fire, and an immediate poll would just see the reset marker
     * still set ("pending"). tt-kmd's userspace sleeps ~2 s here
     * (warm_reset.cpp); 1 s has been ample for one Blackhole. */
    memset(&post_in, 0, sizeof(post_in));
    printf("Waiting for the device to return (polling POST_RESET)...\n");
    Sleep(1000);

    err = 0;
    for (waited = 0; ; waited += 100) {
        if (DeviceIoControl(h, IOCTL_TTWIND_POST_RESET, &post_in,
                            sizeof(post_in), NULL, 0, &returned, NULL)) {
            printf("Reset complete.\n");
            rc = 0;
            break;
        }
        err = GetLastError();
        /* ERROR_BUSY = reset pending (marker not cleared yet) and
         * ERROR_BAD_UNIT = device not back on the bus yet: both are
         * keep-polling statuses. Other errors are retried too - a
         * restore/MMIO-gate failure is retryable by contract. */
        if (waited >= 15000) {
            if (err == ERROR_BUSY) {
                /* Every poll in the whole budget saw the marker still
                 * set: the chip ignored the trigger. This terminal
                 * diagnosis is deliberately made HERE, not in the
                 * driver - one early sample cannot tell "timer has not
                 * fired yet" from "never will" (the v100.3.4 bug). */
                fprintf(stderr, "device ignored the reset trigger "
                        "(marker never cleared in %lu ms)\n", waited);
                rc = 1;
            } else {
                print_win32_error("POST_RESET failed", err);
            }
            break;
        }
        Sleep(100);
    }

out_close:
    CloseHandle(h);
    return rc;
}

/* Print one IOCTL_TTWIND_SYSMEM_STATUS report. */
static void print_sysmem_status(const TTWIND_SYSMEM_STATUS_OUT *st)
{
    static const char *const stage_names[] = {
        "0 (arm never attempted)",
        "1 (allocation failed - no buffer at any tier)",
        "2 (BAR2 iATU / NOC_ID / kernel TLB mapping missing)",
        "3 (PCIe tile detection failed)",
        "4 (iATU programming failed)",
        "5 (loopback verification failed)",
        "6 (verified - sysmem armed)",
    };
    static const char *const tier_names[] = {
        "not tried", "FAILED", "ok",
    };
    char sizebuf[64];
    unsigned i;

    printf("Arm stage       : %s\n",
           st->Stage < 7 ? stage_names[st->Stage] : "?");
    printf("Arm NTSTATUS    : 0x%08x\n", st->LastStatus);
    printf("Allocation      : %s at phys 0x%016llx\n",
           st->TotalSize != 0
               ? format_size(st->TotalSize, sizebuf, sizeof(sizebuf))
               : "none",
           st->SysmemPhys);
    for (i = 0; i < TTWIND_SYSMEM_ALLOC_TIERS; i++) {
        printf("  tier %u        : %-24s %s\n", i,
               format_size(st->TierBytes[i], sizebuf, sizeof(sizebuf)),
               st->TierResult[i] < 3 ? tier_names[st->TierResult[i]] : "?");
    }
    printf("NOC_ID raw      : 0x%08x (x=%u)  -> PCIe tile x = %u\n",
           st->NocIdRaw, st->NocIdRaw & 0x3F, st->PcieTileX);
    printf("Verified        : %s\n", st->Verified ? "yes" : "no");
    if (st->IatuValid) {
        printf("iATU region 0   : base 0x%08x_%08x  target 0x%08x_%08x\n",
               st->Iatu[1], st->Iatu[0], st->Iatu[3], st->Iatu[2]);
        printf("                  limit 0x%08x_%08x  ctrl1 0x%08x  "
               "ctrl2 0x%08x  ctrl3 0x%08x\n",
               st->Iatu[5], st->Iatu[4], st->Iatu[6], st->Iatu[7],
               st->Iatu[8]);
    } else {
        printf("iATU region 0   : not read (restricted or BAR2 unmapped)\n");
    }
    for (i = 0; i < TTWIND_SYSMEM_LOOPBACK_PROBES; i++) {
        const TTWIND_SYSMEM_PROBE *p = &st->Probes[i];

        /* Wrote0 is 0x74744D30+i whenever the probe ran. */
        if (p->Wrote0 == 0) {
            printf("  probe %u       : not reached\n", i);
            continue;
        }
        printf("  probe %u       : off 0x%09llx  wrote %08x %08x  "
               "read %08x %08x  %s\n",
               i, p->Offset, p->Wrote0, p->Wrote1, p->Read0, p->Read1,
               (p->Read0 == p->Wrote0 && p->Read1 == p->Wrote1)
                   ? "ok" : "MISMATCH");
    }
}

/*
 * sysmem: IOCTL_TTWIND_QUERY_SYSMEM plus the SYSMEM_STATUS diagnostic
 * (which stage of the arm the driver got to, allocation tiers, NOC_ID,
 * iATU readback, loopback probes). Exit 0 when sysmem is available, 1
 * when the driver reports it unavailable, 2 on error.
 */
static int cmd_sysmem(void)
{
    TTWIND_QUERY_SYSMEM_OUT q;
    TTWIND_SYSMEM_STATUS_OUT st;
    DWORD returned = 0;
    char sizebuf[64];
    HANDLE h;
    int rc = 2;

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    if (!DeviceIoControl(h, IOCTL_TTWIND_QUERY_SYSMEM, NULL, 0,
                         &q, sizeof(q), &returned, NULL)) {
        print_win32_error("QUERY_SYSMEM failed", GetLastError());
        goto out_close;
    }
    if (returned < sizeof(q)) {
        fprintf(stderr, "short QUERY_SYSMEM reply: %lu bytes\n", returned);
        goto out_close;
    }

    if (q.TotalSize == 0) {
        printf("Sysmem          : unavailable\n");
        rc = 1;
    } else {
        printf("Sysmem          : %s\n",
               format_size(q.TotalSize, sizebuf, sizeof(sizebuf)));
        printf("NOC address     : 0x%016llx\n", q.NocAddress);
        printf("Device I/O addr : 0x%016llx\n", q.DeviceIoAddr);
        printf("Channels        : %u x %s\n", q.ChannelCount,
               format_size(q.ChannelSize, sizebuf, sizeof(sizebuf)));
        printf("Max map bytes   : %s\n",
               format_size(q.MaxMapBytes, sizebuf, sizeof(sizebuf)));
        printf("PCIe tile       : NOC0 (%u, 0)\n", q.PcieTileX);
        rc = 0;
    }

    if (!DeviceIoControl(h, IOCTL_TTWIND_SYSMEM_STATUS, NULL, 0,
                         &st, sizeof(st), &returned, NULL)) {
        /* Diagnostic only; a v100.4.0 driver has no SYSMEM_STATUS. */
        print_win32_error("SYSMEM_STATUS failed", GetLastError());
        goto out_close;
    }
    if (returned < sizeof(st)) {
        fprintf(stderr, "short SYSMEM_STATUS reply: %lu bytes\n",
                returned);
        goto out_close;
    }
    print_sysmem_status(&st);

out_close:
    CloseHandle(h);
    return rc;
}

/*
 * sysmemtest: the end-to-end sysmem acceptance test. QUERY_SYSMEM, MAP
 * the first 1 MiB, write a pattern at offset 0x100 through the cached
 * user view, then read it back OVER THE NOC - through a user TLB window
 * aimed at the PCIe tile at NOC address 4<<58 + 0x100 (the outbound
 * iATU loops it back into the same buffer) - compare, PASS/FAIL,
 * unmap/free everything.
 */
static int cmd_sysmemtest(void)
{
    static const unsigned __int64 test_off = 0x100;
    static const unsigned __int64 win_mask = TTWIND_TLB_WINDOW_SIZE_2M - 1;
    TTWIND_QUERY_SYSMEM_OUT q;
    TTWIND_MAP_SYSMEM_IN smap_in;
    TTWIND_MAP_SYSMEM_OUT smap_out;
    TTWIND_ALLOCATE_TLB_IN alloc_in;
    TTWIND_ALLOCATE_TLB_OUT alloc_out;
    TTWIND_CONFIGURE_TLB_IN cfg_in;
    TTWIND_MAP_TLB_IN tmap_in;
    TTWIND_MAP_TLB_OUT tmap_out;
    TTWIND_UNMAP_BAR_IN unmap_in;
    TTWIND_FREE_TLB_IN free_in;
    volatile unsigned int *sysva;
    volatile unsigned int *nocva;
    unsigned __int64 noc_addr;
    unsigned int expect[4];
    unsigned int got[4];
    int mismatch = 0;
    DWORD returned = 0;
    char sizebuf[64];
    HANDLE h;
    int i;
    int rc = 2;
    int have_tlbmap = 0;

    memset(&smap_out, 0, sizeof(smap_out));
    memset(&tmap_out, 0, sizeof(tmap_out));
    memset(&alloc_out, 0, sizeof(alloc_out));

    h = open_first_device();
    if (h == INVALID_HANDLE_VALUE)
        return 2;

    if (!DeviceIoControl(h, IOCTL_TTWIND_QUERY_SYSMEM, NULL, 0,
                         &q, sizeof(q), &returned, NULL)) {
        print_win32_error("QUERY_SYSMEM failed", GetLastError());
        goto out_close;
    }
    if (q.TotalSize == 0) {
        fprintf(stderr, "sysmem unavailable - FAIL\n");
        goto out_close;
    }
    printf("Sysmem %s at NOC 0x%016llx, PCIe tile (%u, 0)\n",
           format_size(q.TotalSize, sizebuf, sizeof(sizebuf)),
           q.NocAddress, q.PcieTileX);

    /* Map the first 1 MiB of sysmem (cached, contiguous user VA). */
    memset(&smap_in, 0, sizeof(smap_in));
    smap_in.Offset = 0;
    smap_in.Length = 1024 * 1024;
    if (!DeviceIoControl(h, IOCTL_TTWIND_MAP_SYSMEM, &smap_in,
                         sizeof(smap_in), &smap_out, sizeof(smap_out),
                         &returned, NULL)) {
        print_win32_error("MAP_SYSMEM failed", GetLastError());
        goto out_close;
    }
    printf("Mapped %s at user VA 0x%llx\n",
           format_size(smap_out.Length, sizebuf, sizeof(sizebuf)),
           smap_out.UserVa);

    /* Write the pattern through the cached view. */
    sysva = (volatile unsigned int *)(ULONG_PTR)
            (smap_out.UserVa + test_off);
    for (i = 0; i < 4; i++) {
        expect[i] = 0x74747335u + (unsigned int)i * 0x01010101u;
        sysva[i] = expect[i];
    }

    /* TLB window at the PCIe tile covering NOC 4<<58 + test_off. */
    noc_addr = q.NocAddress + test_off;

    memset(&alloc_in, 0, sizeof(alloc_in));
    alloc_in.Size = TTWIND_TLB_WINDOW_SIZE_2M;
    if (!DeviceIoControl(h, IOCTL_TTWIND_ALLOCATE_TLB, &alloc_in,
                         sizeof(alloc_in), &alloc_out, sizeof(alloc_out),
                         &returned, NULL)) {
        print_win32_error("ALLOCATE_TLB failed", GetLastError());
        goto out_unmap_sysmem;
    }

    memset(&cfg_in, 0, sizeof(cfg_in));
    cfg_in.TlbId = alloc_out.TlbId;
    cfg_in.Config.Addr = noc_addr & ~win_mask;
    cfg_in.Config.XEnd = (unsigned short)q.PcieTileX;
    cfg_in.Config.YEnd = 0;
    cfg_in.Config.Noc = 0;
    cfg_in.Config.Ordering = 1; /* strict */
    if (!DeviceIoControl(h, IOCTL_TTWIND_CONFIGURE_TLB, &cfg_in,
                         sizeof(cfg_in), NULL, 0, &returned, NULL)) {
        print_win32_error("CONFIGURE_TLB failed", GetLastError());
        goto out_free_tlb;
    }

    memset(&tmap_in, 0, sizeof(tmap_in));
    tmap_in.TlbId = alloc_out.TlbId;
    tmap_in.CacheMode = TTWIND_CACHE_UC;
    if (!DeviceIoControl(h, IOCTL_TTWIND_MAP_TLB, &tmap_in,
                         sizeof(tmap_in), &tmap_out, sizeof(tmap_out),
                         &returned, NULL)) {
        print_win32_error("MAP_TLB failed", GetLastError());
        goto out_free_tlb;
    }
    have_tlbmap = 1;

    /* Read back over the NOC and compare. */
    nocva = (volatile unsigned int *)(ULONG_PTR)
            (tmap_out.UserVa + (noc_addr & win_mask));
    for (i = 0; i < 4; i++) {
        got[i] = nocva[i];
        printf("  [0x%llx] wrote 0x%08x  NOC(0x%016llx) read 0x%08x  %s\n",
               test_off + 4ull * i, expect[i], noc_addr + 4ull * i,
               got[i], got[i] == expect[i] ? "ok" : "MISMATCH");
        if (got[i] != expect[i])
            mismatch = 1;
    }

    /* Clean the probe bytes up again. */
    for (i = 0; i < 4; i++)
        sysva[i] = 0;

    if (mismatch) {
        printf("sysmemtest: FAIL\n");
        rc = 1;
    } else {
        printf("sysmemtest: PASS\n");
        rc = 0;
    }

    memset(&unmap_in, 0, sizeof(unmap_in));
    unmap_in.UserVa = tmap_out.UserVa;
    if (!DeviceIoControl(h, IOCTL_TTWIND_UNMAP_BAR, &unmap_in,
                         sizeof(unmap_in), NULL, 0, &returned, NULL)) {
        print_win32_error("UNMAP_BAR(tlb) failed", GetLastError());
        rc = 2;
    }
    have_tlbmap = 0;

out_free_tlb:
    if (have_tlbmap) {
        memset(&unmap_in, 0, sizeof(unmap_in));
        unmap_in.UserVa = tmap_out.UserVa;
        DeviceIoControl(h, IOCTL_TTWIND_UNMAP_BAR, &unmap_in,
                        sizeof(unmap_in), NULL, 0, &returned, NULL);
    }
    memset(&free_in, 0, sizeof(free_in));
    free_in.TlbId = alloc_out.TlbId;
    if (!DeviceIoControl(h, IOCTL_TTWIND_FREE_TLB, &free_in,
                         sizeof(free_in), NULL, 0, &returned, NULL)) {
        print_win32_error("FREE_TLB failed", GetLastError());
        rc = 2;
    }

out_unmap_sysmem:
    memset(&unmap_in, 0, sizeof(unmap_in));
    unmap_in.UserVa = smap_out.UserVa;
    if (!DeviceIoControl(h, IOCTL_TTWIND_UNMAP_BAR, &unmap_in,
                         sizeof(unmap_in), NULL, 0, &returned, NULL)) {
        print_win32_error("UNMAP_BAR(sysmem) failed", GetLastError());
        rc = 2;
    }

out_close:
    CloseHandle(h);
    return rc;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: ttwind-info                       list devices\n"
            "       ttwind-info bar0read <offset>     read u32 from BAR0\n"
            "       ttwind-info tlbread <x> <y> <addr> read u32 via TLB window\n"
            "       ttwind-info tlbreadx addr=<a> xend=<x> yend=<y> [xstart=] [ystart=]\n"
            "                   [noc=] [mcast=] [ordering=] [linked=] [staticvc=] [wc=]\n"
            "                                         read u32, every TLB field settable\n"
            "       ttwind-info arcmsg <hdr> [w1..w7] send raw ARC/SMC message\n"
            "       ttwind-info arcstatus             probe ARC queue discovery\n"
            "       ttwind-info sysmem                query host sysmem buffer\n"
            "       ttwind-info sysmemtest            sysmem write + NOC-readback test\n"
            "       ttwind-info reset                 reset the device\n");
    return 2;
}

int main(int argc, char **argv)
{
    wchar_t *list;
    wchar_t *p;
    unsigned found = 0, failed = 0;

    if (argc >= 2) {
        if (strcmp(argv[1], "bar0read") == 0 && argc == 3)
            return cmd_bar0read(argv[2]);
        if (strcmp(argv[1], "tlbread") == 0 && argc == 5)
            return cmd_tlbread(argv[2], argv[3], argv[4]);
        if (strcmp(argv[1], "tlbreadx") == 0 && argc >= 3)
            return cmd_tlbreadx(argc - 2, argv + 2);
        if (strcmp(argv[1], "arcmsg") == 0 && argc >= 3 && argc <= 10)
            return cmd_arcmsg(argc - 2, argv + 2);
        if (strcmp(argv[1], "arcstatus") == 0 && argc == 2)
            return cmd_arcstatus();
        if (strcmp(argv[1], "sysmem") == 0 && argc == 2)
            return cmd_sysmem();
        if (strcmp(argv[1], "sysmemtest") == 0 && argc == 2)
            return cmd_sysmemtest();
        if (strcmp(argv[1], "reset") == 0 && argc == 2)
            return cmd_reset();
        return usage();
    }

    list = get_interface_list();
    if (list == NULL)
        return 2;

    /* The list is a sequence of NUL-terminated strings ended by an
     * empty string. */
    for (p = list; *p != L'\0'; p += wcslen(p) + 1) {
        if (query_device(p, found) != 0)
            failed++;
        found++;
    }
    free(list);

    if (found == 0) {
        printf("No tt-wind devices found.\n");
        return 1;
    }
    return failed != 0 ? 2 : 0;
}
