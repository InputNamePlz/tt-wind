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

static int usage(void)
{
    fprintf(stderr,
            "usage: ttwind-info                       list devices\n"
            "       ttwind-info bar0read <offset>     read u32 from BAR0\n"
            "       ttwind-info tlbread <x> <y> <addr> read u32 via TLB window\n");
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
