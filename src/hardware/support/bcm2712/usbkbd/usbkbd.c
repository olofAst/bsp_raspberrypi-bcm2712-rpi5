/*
 * usbkbd-tty - feed USB keyboard input into a tty.
 *
 * QNX 7.1 has no text console for this board, so the shell lives on a serial
 * port. This small HID client turns key presses of a USB keyboard into
 * characters and injects them into that port's input with tcinject(), as if
 * they had been received on the serial line.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <sys/queue.h>
#include <sys/hiddi.h>
/* <sys/hidut.h> of SDP 7.1 pulls in a header that is not shipped; only this is needed */
#define HID_INPUT_REPORT         0x08

#define USAGE_PAGE_DESKTOP      0x01
#define USAGE_KEYBOARD          0x06
#define USAGE_KEYPAD            0x07
#define USAGE_PAGE_KEYBOARD     0x07
#define MAX_KEYS                16

static int      tty_fd = -1;
static int      verbose;

/* one keyboard at a time is plenty for a console */
static struct hidd_report_instance  *kbd_report_inst;
static struct hidd_device_instance  *kbd_device;
static uint16_t                      prev_keys[MAX_KEYS];
static uint16_t                      prev_num;

/* HID keyboard usage 0x04.. -> character, US layout: unshifted / shifted */
static const char unshifted[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\033\177\t -=[]\\#;'`,./";
static const char shifted[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\033\177\t _+{}|~:\"~<>?";

static void inject(const char *s, int n)
{
    /*
     * A negative count selects the raw input queue: the characters then go
     * through the normal tty processing (echo, line editing, CR->NL) and are
     * also seen by programs that read the tty in raw mode, such as the ksh
     * line editor. A positive count would write to the canonical queue only.
     */
    if (tcinject(tty_fd, (char *)s, -n) == -1) {
        fprintf(stderr, "usbkbd-tty: tcinject: %s\n", strerror(errno));
    }
}

static void key_pressed(uint16_t usage, int shift, int ctrl)
{
    char c;

    if (verbose) {
        fprintf(stderr, "usbkbd-tty: key 0x%02x%s%s\n", usage, shift ? " shift" : "", ctrl ? " ctrl" : "");
    }
    switch (usage) {
        case 0x4f: inject("\033[C", 3); return;     /* right */
        case 0x50: inject("\033[D", 3); return;     /* left */
        case 0x51: inject("\033[B", 3); return;     /* down */
        case 0x52: inject("\033[A", 3); return;     /* up */
        case 0x4c: inject("\177", 1); return;       /* delete */
        case 0x58: inject("\r", 1); return;         /* keypad enter */
        default: break;
    }
    if ((usage < 0x04) || (usage >= 0x04 + (sizeof(unshifted) - 1))) {
        return;
    }
    c = shift ? shifted[usage - 0x04] : unshifted[usage - 0x04];
    if (ctrl) {
        c &= 0x1f;
    }
    inject(&c, 1);
}

static void on_report(struct hidd_connection *conn, struct hidd_report *handle, void *data,
                      _Uint32t len, _Uint32t flags, void *user)
{
    uint16_t keys[MAX_KEYS];
    uint16_t num = MAX_KEYS;
    uint16_t i, j;
    int shift = 0, ctrl = 0;

    if (kbd_report_inst == NULL) {
        return;
    }
    if (hidd_get_buttons(kbd_report_inst, NULL, USAGE_PAGE_KEYBOARD, data, keys, &num) != EOK) {
        return;
    }
    /* modifiers are reported as keys 0xe0..0xe7 */
    for (i = 0; i < num; i++) {
        if ((keys[i] == 0xe1) || (keys[i] == 0xe5)) shift = 1;
        if ((keys[i] == 0xe0) || (keys[i] == 0xe4)) ctrl = 1;
    }
    /* anything down now that was not down in the previous report is a new press */
    for (i = 0; i < num; i++) {
        int was_down = 0;
        if ((keys[i] >= 0xe0) || (keys[i] < 0x04)) {
            continue;
        }
        for (j = 0; j < prev_num; j++) {
            if (prev_keys[j] == keys[i]) {
                was_down = 1;
                break;
            }
        }
        if (!was_down) {
            key_pressed(keys[i], shift, ctrl);
        }
    }
    memcpy(prev_keys, keys, sizeof(keys));
    prev_num = num;
}

static void on_insertion(struct hidd_connection *conn, hidd_device_instance_t *dev)
{
    struct hidd_collection **cols;
    uint16_t num_cols, i;

    if (kbd_report_inst != NULL) {
        return;     /* already have a keyboard */
    }
    if (hidd_get_collections(dev, NULL, &cols, &num_cols) != EOK) {
        return;
    }
    for (i = 0; i < num_cols; i++) {
        uint16_t page, usage;
        struct hidd_report_instance *ri;
        struct hidd_report *report;

        if (hidd_collection_usage(cols[i], &page, &usage) != EOK) {
            continue;
        }
        if ((page != USAGE_PAGE_DESKTOP) || ((usage != USAGE_KEYBOARD) && (usage != USAGE_KEYPAD))) {
            continue;
        }
        if (hidd_get_report_instance(cols[i], 0, HID_INPUT_REPORT, &ri) != EOK) {
            continue;
        }
        if (hidd_report_attach(conn, dev, ri, 0, 0, &report) != EOK) {
            fprintf(stderr, "usbkbd-tty: cannot attach to the keyboard report\n");
            continue;
        }
        kbd_report_inst = ri;
        kbd_device = dev;
        prev_num = 0;
        fprintf(stderr, "usbkbd-tty: keyboard %04x:%04x attached\n",
                dev->device_ident.vendor_id, dev->device_ident.product_id);
        return;
    }
}

static void on_removal(struct hidd_connection *conn, hidd_device_instance_t *dev)
{
    if (dev == kbd_device) {
        hidd_reports_detach(conn, dev);
        kbd_report_inst = NULL;
        kbd_device = NULL;
        fprintf(stderr, "usbkbd-tty: keyboard removed\n");
    }
}

int main(int argc, char *argv[])
{
    static hidd_device_ident_t  interest = { HIDD_CONNECT_WILDCARD, HIDD_CONNECT_WILDCARD, HIDD_CONNECT_WILDCARD };
    static hidd_funcs_t         funcs = { _HIDDI_NFUNCS, on_insertion, on_removal, on_report, NULL };
    hidd_connect_parm_t         parm;
    struct hidd_connection     *conn;
    const char                 *tty = "/dev/ser10";
    int                         opt;

    while ((opt = getopt(argc, argv, "v")) != -1) {
        if (opt == 'v') {
            verbose++;
        } else {
            return EXIT_FAILURE;
        }
    }
    if (optind < argc) {
        tty = argv[optind];
    }

    /*
     * Become a daemon in a session of its own, with no controlling terminal.
     * Otherwise, once a login shell owns the tty, this process counts as a
     * background job of that terminal and tcinject() stops it with SIGTTOU.
     */
    if (daemon(1, 1) == -1) {
        fprintf(stderr, "usbkbd-tty: daemon: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    tty_fd = open(tty, O_RDWR | O_NOCTTY);
    if (tty_fd == -1) {
        fprintf(stderr, "usbkbd-tty: %s: %s\n", tty, strerror(errno));
        return EXIT_FAILURE;
    }

    memset(&parm, 0, sizeof(parm));
    parm.path = NULL;
    parm.vhid = HID_VERSION;
    parm.vhidd = HIDD_VERSION;
    parm.device_ident = &interest;
    parm.funcs = &funcs;
    parm.connect_wait = 10;

    if (hidd_connect(&parm, &conn) != EOK) {
        fprintf(stderr, "usbkbd-tty: cannot connect to io-hid: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    for (;;) {
        pause();
    }
    return EXIT_SUCCESS;
}
