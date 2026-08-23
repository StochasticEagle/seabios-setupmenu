// Interactive BIOS Setup utility.
//
// Copyright (C) 2026 StochasticEagle contributors
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "bregs.h" // struct bregs
#include "config.h" // CONFIG_*
#include "fw/paravirt.h" // RamSize
#include "hw/rtc.h" // rtc_read
#include "output.h" // printf
#include "romfile.h" // romfile_loadbool
#include "setup.h"
#include "stacks.h" // call16_int
#include "string.h" // memset
#include "util.h" // get_keystroke_full
#include "x86.h" // cpuid

#define SETUP_FWCFG_PATH "opt/org.seabios/setup"

#define KEY_ESC     0x011b
#define KEY_ENTER   0x1c0d
#define KEY_F10     0x4400

static void
setup_int10(struct bregs *br)
{
    br->flags = F_IF;
    call16_int(0x10, br);
}

static void
setup_set_mode3(void)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.ax = 0x0003;
    setup_int10(&br);
}

static void
setup_set_cursor(u8 row, u8 col)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.ah = 0x02;
    br.bh = 0;
    br.dh = row;
    br.dl = col;
    setup_int10(&br);
}

static void
setup_clear_screen(void)
{
    setup_set_mode3();
}

static void
setup_write_at(u8 row, u8 col, const char *text)
{
    setup_set_cursor(row, col);
    printf("%s", text);
}

static void
setup_draw_frame(void)
{
    setup_clear_screen();
    setup_write_at(0, 2, "SeaBIOS Setup Utility");
    setup_write_at(1, 0, "------------------------------------------------------------------------------");
    setup_write_at(22, 0, "------------------------------------------------------------------------------");
    setup_write_at(23, 1, "Enter: Select   Esc: Exit   F10: Save and Exit");
}

static u8
bcd_to_bin(u8 value)
{
    return (value >> 4) * 10 + (value & 0x0f);
}

static void
setup_get_datetime(char *date, int datesize, char *time, int timesize)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ah = 0x04;
    call16_int(0x1a, &br);
    if (br.flags & F_CF) {
        snprintf(date, datesize, "Unavailable");
    } else {
        snprintf(date, datesize, "%02u/%02u/%02u%02u"
                 , bcd_to_bin(br.dh), bcd_to_bin(br.dl)
                 , bcd_to_bin(br.ch), bcd_to_bin(br.cl));
    }

    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    br.ah = 0x02;
    call16_int(0x1a, &br);
    if (br.flags & F_CF) {
        snprintf(time, timesize, "Unavailable");
    } else {
        snprintf(time, timesize, "%02u:%02u:%02u"
                 , bcd_to_bin(br.ch), bcd_to_bin(br.cl)
                 , bcd_to_bin(br.dh));
    }
}

static void
setup_get_cpu_name(char *buf, int size)
{
    u32 eax, ebx, ecx, edx;
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004 && size >= 49) {
        int offset = 0;
        u32 leaf;
        for (leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, &eax, &ebx, &ecx, &edx);
            memcpy(buf + offset, &eax, 4); offset += 4;
            memcpy(buf + offset, &ebx, 4); offset += 4;
            memcpy(buf + offset, &ecx, 4); offset += 4;
            memcpy(buf + offset, &edx, 4); offset += 4;
        }
        buf[48] = 0;

        char *start = buf;
        while (*start == ' ')
            start++;
        if (start != buf)
            memmove(buf, start, strlen(start) + 1);
        int len = strlen(buf);
        while (len && buf[len - 1] == ' ')
            buf[--len] = 0;
        return;
    }

    cpuid(0, &eax, &ebx, &ecx, &edx);
    if (size < 13) {
        if (size)
            buf[0] = 0;
        return;
    }
    memcpy(buf, &ebx, 4);
    memcpy(buf + 4, &edx, 4);
    memcpy(buf + 8, &ecx, 4);
    buf[12] = 0;
}

static const char *
setup_floppy_type(u8 type)
{
    switch (type) {
    case 0: return "Not Present";
    case 1: return "5.25 inch, 360 KB";
    case 2: return "5.25 inch, 1200 KB";
    case 3: return "3.5 inch, 720 KB";
    case 4: return "3.5 inch, 1440 KB";
    case 5: return "3.5 inch, 2880 KB";
    case 6: return "5.25 inch, 160 KB";
    case 7: return "5.25 inch, 180 KB";
    case 8: return "5.25 inch, 320 KB";
    default: return "Unknown";
    }
}

static void
setup_draw_main(void)
{
    char date[16], time[16], cpu[49], line[80];
    setup_get_datetime(date, sizeof(date), time, sizeof(time));
    setup_get_cpu_name(cpu, sizeof(cpu));

    u64 memory = (u64)RamSize + RamSizeOver4G;
    u32 memory_mb = memory >> 20;
    u16 cpus = qemu_get_present_cpus_count();
    u8 floppy = CONFIG_FLOPPY ? rtc_read(CMOS_FLOPPY_DRIVE_TYPE) : 0;

    setup_draw_frame();
    setup_write_at(3, 3, "Main");

    snprintf(line, sizeof(line), "System Date:     %s", date);
    setup_write_at(5, 5, line);
    snprintf(line, sizeof(line), "System Time:     %s", time);
    setup_write_at(6, 5, line);

    snprintf(line, sizeof(line), "CPU:             %s", cpu);
    setup_write_at(8, 5, line);
    snprintf(line, sizeof(line), "Processor(s):    %u", cpus);
    setup_write_at(9, 5, line);
    snprintf(line, sizeof(line), "System Memory:   %u MB", memory_mb);
    setup_write_at(10, 5, line);

    snprintf(line, sizeof(line), "Floppy A:        %s"
             , CONFIG_FLOPPY ? setup_floppy_type(floppy >> 4) : "Disabled");
    setup_write_at(12, 5, line);
    snprintf(line, sizeof(line), "Floppy B:        %s"
             , CONFIG_FLOPPY ? setup_floppy_type(floppy & 0x0f) : "Disabled");
    setup_write_at(13, 5, line);

    setup_write_at(20, 5, "> Exit Setup");
}

int
setup_requested(void)
{
    if (!CONFIG_QEMU)
        return 0;
    return romfile_loadbool(SETUP_FWCFG_PATH, 0);
}

void
setup_run(void)
{
    if (!CONFIG_QEMU)
        return;

    setup_draw_main();
    for (;;) {
        int key = get_keystroke_full(1000);
        switch (key) {
        case KEY_ENTER:
        case KEY_ESC:
        case KEY_F10:
            return;
        }
    }
}
