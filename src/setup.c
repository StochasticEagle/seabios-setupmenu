// Interactive BIOS Setup utility.
//
// Copyright (C) 2026 StochasticEagle contributors
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "bregs.h" // struct bregs
#include "config.h" // CONFIG_*
#include "output.h" // printf
#include "romfile.h" // romfile_loadbool
#include "setup.h"
#include "stacks.h" // call16_int
#include "string.h" // memset
#include "util.h" // get_keystroke_full

#define SETUP_FWCFG_PATH "opt/org.seabios/setup"

#define KEY_ESC     0x011b
#define KEY_ENTER   0x1c0d
#define KEY_UP      0x4800
#define KEY_DOWN    0x5000
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
    setup_write_at(23, 1, "Up/Down: Select   Enter: Open   Esc: Exit   F10: Save and Exit");
}

static void
setup_draw_main(int selected)
{
    static const char *items[] = {
        "Main / System Information",
        "Exit",
    };
    int i;

    setup_draw_frame();
    setup_write_at(3, 3, "BIOS Setup");
    setup_write_at(5, 5, "Use the arrow keys to select an item.");

    for (i = 0; i < ARRAY_SIZE(items); i++) {
        setup_set_cursor(8 + i * 2, 7);
        printf("%c %s", i == selected ? '>' : ' ', items[i]);
    }
}

static void
setup_draw_system_info(void)
{
    setup_draw_frame();
    setup_write_at(3, 3, "Main / System Information");
    setup_write_at(6, 5, "Firmware: SeaBIOS");
    setup_write_at(8, 5, "BIOS Setup prototype");
    setup_write_at(20, 5, "Press Esc to return.");
}

static int
setup_system_info_page(void)
{
    setup_draw_system_info();
    for (;;) {
        int key = get_keystroke_full(1000);
        if (key == KEY_ESC)
            return 0;
        if (key == KEY_F10)
            return 1;
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

    int selected = 0;
    setup_draw_main(selected);

    for (;;) {
        int key = get_keystroke_full(1000);
        switch (key) {
        case KEY_UP:
            if (selected > 0)
                selected--;
            setup_draw_main(selected);
            break;
        case KEY_DOWN:
            if (selected < 1)
                selected++;
            setup_draw_main(selected);
            break;
        case KEY_ENTER:
            if (selected == 0) {
                if (setup_system_info_page())
                    return;
                setup_draw_main(selected);
            } else {
                return;
            }
            break;
        case KEY_ESC:
        case KEY_F10:
            return;
        }
    }
}
