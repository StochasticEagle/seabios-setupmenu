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
#include "hw/ata.h" // struct atadrive_s
#include "x86.h" // cpuid

// setup.c must call the normal boot registration functions after recording
// the device.  ata.h redirects ATA's registration calls to the hooks below.
#undef boot_add_hd
#undef boot_add_cd

#define SETUP_FWCFG_PATH "opt/org.seabios/setup"

#define KEY_ESC     0x011b
#define KEY_ENTER   0x1c0d
#define KEY_F10     0x4400
#define KEY_UP      0x4800
#define KEY_DOWN    0x5000

#define SETUP_BACK     0
#define SETUP_SAVE     1
#define SETUP_DISCARD  2

#define TEXT_VRAM      ((u16*)0x000b8000)
#define TEXT_CELLS     (80 * 25)
#define TEXT_BLANK     0x0720

struct setup_ata_slot {
    struct drive_s *drive;
    const char *description;
};

static struct setup_ata_slot SetupAta[2][2];

static void
setup_record_ata(struct drive_s *drive, const char *desc)
{
    struct atadrive_s *adrive = container_of(drive, struct atadrive_s, drive);
    u8 ataid = adrive->chan_gf->ataid;
    u8 slave = adrive->slave;

    if (ataid >= ARRAY_SIZE(SetupAta) || slave >= ARRAY_SIZE(SetupAta[0]))
        return;
    SetupAta[ataid][slave].drive = drive;
    SetupAta[ataid][slave].description = desc;
}

void
ata_inventory_add_hd(struct drive_s *drive, const char *desc, int prio)
{
    setup_record_ata(drive, desc);
    boot_add_hd(drive, desc, prio);
}

void
ata_inventory_add_cd(struct drive_s *drive, const char *desc, int prio)
{
    setup_record_ata(drive, desc);
    boot_add_cd(drive, desc, prio);
}

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
setup_blank_screen(void)
{
    // Blank the visible 80x25 page with normal text attributes.  Clearing
    // the bytes to zero would leave attribute 0x00 (black on black), which
    // can make subsequent SeaBIOS or DOS output appear to hang invisibly.
    int i;
    for (i = 0; i < TEXT_CELLS; i++)
        TEXT_VRAM[i] = TEXT_BLANK;
    setup_set_cursor(0, 0);
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
    setup_write_at(23, 1, "Up/Down: Select  +/-: Change  Enter: Open  Esc: Back/Exit  F10: Save/Exit");
}

static u8
bcd_to_bin(u8 value)
{
    return (value >> 4) * 10 + (value & 0x0f);
}

static u8
setup_rtc_value(u8 value, u8 statusb)
{
    if (statusb & RTC_B_BIN)
        return value;
    return bcd_to_bin(value);
}

static void
setup_get_datetime(char *date, int datesize, char *time, int timesize)
{
    if (rtc_updating()) {
        snprintf(date, datesize, "Unavailable");
        snprintf(time, timesize, "Unavailable");
        return;
    }

    u8 statusb = rtc_read(CMOS_STATUS_B);
    u8 second = setup_rtc_value(rtc_read(CMOS_RTC_SECONDS), statusb);
    u8 minute = setup_rtc_value(rtc_read(CMOS_RTC_MINUTES), statusb);
    u8 hour = rtc_read(CMOS_RTC_HOURS);
    u8 day = setup_rtc_value(rtc_read(CMOS_RTC_DAY_MONTH), statusb);
    u8 month = setup_rtc_value(rtc_read(CMOS_RTC_MONTH), statusb);
    u8 year = setup_rtc_value(rtc_read(CMOS_RTC_YEAR), statusb);
    u8 century = setup_rtc_value(rtc_read(CMOS_CENTURY), statusb);

    if (!(statusb & RTC_B_24HR)) {
        u8 pm = hour & 0x80;
        hour &= 0x7f;
        hour = setup_rtc_value(hour, statusb);
        if (pm && hour < 12)
            hour += 12;
        else if (!pm && hour == 12)
            hour = 0;
    } else {
        hour = setup_rtc_value(hour, statusb);
    }

    snprintf(date, datesize, "%02u/%02u/%02u%02u"
             , month, day, century, year);
    // SeaBIOS's formatter does not implement the libc '0' width flag, so
    // emit each time digit explicitly to guarantee HH:MM:SS formatting.
    snprintf(time, timesize, "%u%u:%u%u:%u%u"
             , hour / 10, hour % 10, minute / 10, minute % 10
             , second / 10, second % 10);
}

static void
setup_update_datetime(void)
{
    char date[16], time[16], line[40];
    setup_get_datetime(date, sizeof(date), time, sizeof(time));

    snprintf(line, sizeof(line), "System Date:     %s", date);
    setup_write_at(5, 5, line);
    snprintf(line, sizeof(line), "System Time:     %s", time);
    setup_write_at(6, 5, line);
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

static const char *
setup_ata_description(u8 ataid, u8 slave)
{
    const char *desc = SetupAta[ataid][slave].description;
    return desc ? desc : "Not Present";
}

static const char *
setup_boot_name(u8 value)
{
    switch (value) {
    case BOOT_ORDER_FLOPPY: return "Floppy";
    case BOOT_ORDER_HD:     return "Hard Disk";
    case BOOT_ORDER_CD:     return "CD-ROM";
    case BOOT_ORDER_BEV:    return "Network / Option ROM";
    default:                return "None";
    }
}

static int
setup_boot_used(const u8 order[3], int selected, u8 value)
{
    if (value == BOOT_ORDER_NONE)
        return 0;
    int i;
    for (i = 0; i < 3; i++)
        if (i != selected && order[i] == value)
            return 1;
    return 0;
}

static void
setup_boot_cycle(u8 order[3], int selected, int direction)
{
    int value = order[selected];
    do {
        value += direction;
        if (value > BOOT_ORDER_BEV)
            value = BOOT_ORDER_NONE;
        else if (value < BOOT_ORDER_NONE)
            value = BOOT_ORDER_BEV;
    } while (setup_boot_used(order, selected, value));
    order[selected] = value;
}

static void
setup_draw_main(int selected)
{
    char cpu[49], line[80];
    setup_get_cpu_name(cpu, sizeof(cpu));

    u64 memory = (u64)RamSize + RamSizeOver4G;
    u32 memory_mb = memory >> 20;
    u16 cpus = qemu_get_present_cpus_count();
    u8 floppy = CONFIG_FLOPPY ? rtc_read(CMOS_FLOPPY_DRIVE_TYPE) : 0;

    setup_draw_frame();
    setup_write_at(3, 3, "Main");
    setup_update_datetime();

    snprintf(line, sizeof(line), "CPU:             %s", cpu);
    setup_write_at(8, 5, line);
    snprintf(line, sizeof(line), "Processor(s):    %u", cpus);
    setup_write_at(9, 5, line);
    snprintf(line, sizeof(line), "System Memory:   %u MB", memory_mb);
    setup_write_at(10, 5, line);

    snprintf(line, sizeof(line), "Primary Master:  %s", setup_ata_description(0, 0));
    setup_write_at(12, 5, line);
    snprintf(line, sizeof(line), "Primary Slave:   %s", setup_ata_description(0, 1));
    setup_write_at(13, 5, line);
    snprintf(line, sizeof(line), "Secondary Master:%s", setup_ata_description(1, 0));
    setup_write_at(14, 5, line);
    snprintf(line, sizeof(line), "Secondary Slave: %s", setup_ata_description(1, 1));
    setup_write_at(15, 5, line);

    snprintf(line, sizeof(line), "Floppy A:        %s"
             , CONFIG_FLOPPY ? setup_floppy_type(floppy >> 4) : "Disabled");
    setup_write_at(17, 5, line);
    snprintf(line, sizeof(line), "Floppy B:        %s"
             , CONFIG_FLOPPY ? setup_floppy_type(floppy & 0x0f) : "Disabled");
    setup_write_at(18, 5, line);

    setup_set_cursor(20, 5);
    printf("%c Boot Configuration", selected == 0 ? '>' : ' ');
    setup_set_cursor(21, 5);
    printf("%c Exit Setup", selected == 1 ? '>' : ' ');
}

static void
setup_draw_boot(const u8 order[3], int selected)
{
    char line[80];
    setup_draw_frame();
    setup_write_at(3, 3, "Boot Configuration");
    setup_write_at(5, 5, "Choose the boot device class for each priority slot.");
    setup_write_at(6, 5, "Use +/- or Enter to change the selected value.");

    int i;
    for (i = 0; i < 3; i++) {
        snprintf(line, sizeof(line), "%c Boot Option %u:  %s"
                 , selected == i ? '>' : ' ', i + 1, setup_boot_name(order[i]));
        setup_write_at(9 + i * 2, 7, line);
    }
    setup_write_at(17, 5, "Changes remain staged until Save and Exit.");
}

static int
setup_boot_page(u8 order[3])
{
    int selected = 0;
    setup_draw_boot(order, selected);
    for (;;) {
        int key = get_keystroke_full(1000);
        int redraw = 0;
        if (key == KEY_UP) {
            if (selected > 0) {
                selected--;
                redraw = 1;
            }
        } else if (key == KEY_DOWN) {
            if (selected < 2) {
                selected++;
                redraw = 1;
            }
        } else if (key == KEY_ENTER || (key & 0xff) == '+') {
            setup_boot_cycle(order, selected, 1);
            redraw = 1;
        } else if ((key & 0xff) == '-') {
            setup_boot_cycle(order, selected, -1);
            redraw = 1;
        } else if (key == KEY_ESC) {
            return SETUP_BACK;
        } else if (key == KEY_F10) {
            return SETUP_SAVE;
        }
        if (redraw)
            setup_draw_boot(order, selected);
    }
}

static void
setup_draw_exit(int selected)
{
    static const char *items[] = {
        "Save Changes and Exit",
        "Discard Changes and Exit",
        "Return to Setup",
    };
    setup_draw_frame();
    setup_write_at(3, 3, "Exit");

    int i;
    for (i = 0; i < ARRAY_SIZE(items); i++) {
        setup_set_cursor(8 + i * 2, 7);
        printf("%c %s", selected == i ? '>' : ' ', items[i]);
    }
}

static int
setup_exit_page(void)
{
    int selected = 0;
    setup_draw_exit(selected);
    for (;;) {
        int key = get_keystroke_full(1000);
        int redraw = 0;
        if (key == KEY_UP) {
            if (selected > 0) {
                selected--;
                redraw = 1;
            }
        } else if (key == KEY_DOWN) {
            if (selected < 2) {
                selected++;
                redraw = 1;
            }
        } else if (key == KEY_ENTER) {
            if (selected == 0)
                return SETUP_SAVE;
            if (selected == 1)
                return SETUP_DISCARD;
            return SETUP_BACK;
        } else if (key == KEY_ESC) {
            return SETUP_BACK;
        } else if (key == KEY_F10) {
            return SETUP_SAVE;
        }
        if (redraw)
            setup_draw_exit(selected);
    }
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

    u8 order[3];
    boot_get_order(order);
    int i;
    for (i = 0; i < 3; i++)
        if (order[i] > BOOT_ORDER_BEV)
            order[i] = BOOT_ORDER_NONE;

    int selected = 0;
    setup_draw_main(selected);
    for (;;) {
        int key = get_keystroke_full(1000);
        if (key < 0) {
            // Refresh only the two RTC fields.  Do not reset mode 3 or redraw
            // the page, because repeated VGA mode sets visibly flash in QEMU.
            setup_update_datetime();
        } else if (key == KEY_UP) {
            if (selected > 0) {
                selected--;
                setup_draw_main(selected);
            }
        } else if (key == KEY_DOWN) {
            if (selected < 1) {
                selected++;
                setup_draw_main(selected);
            }
        } else if (key == KEY_ENTER) {
            int action;
            if (selected == 0)
                action = setup_boot_page(order);
            else
                action = setup_exit_page();
            if (action == SETUP_SAVE) {
                boot_set_order(order);
                setup_blank_screen();
                return;
            }
            if (action == SETUP_DISCARD) {
                setup_blank_screen();
                return;
            }
            setup_draw_main(selected);
        } else if (key == KEY_ESC) {
            setup_blank_screen();
            return;
        } else if (key == KEY_F10) {
            boot_set_order(order);
            setup_blank_screen();
            return;
        }
    }
}
