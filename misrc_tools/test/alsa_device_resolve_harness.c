/*
 * Unit harness for the dongle's ALSA capture-device resolver.
 *
 * The stream's audio must come from the SAME physical device as its picture,
 * or A/V sync stops being free and starts being a project. The only stable way
 * to say "same device" is the USB bus address, which V4L2 reports verbatim in
 * VIDIOC_QUERYCAP's bus_info and ALSA prints in /proc/asound/cards:
 *
 *   v4l2   Bus info : usb-0000:00:14.0-13.3
 *   asound MacroSilicon MS210x at usb-0000:00:14.0-13.3, high speed
 *
 * Card INDICES are never used: they move across reboots and replugs, so a
 * stored hw:3,0 breaks intermittently -- the worst failure shape there is.
 *
 * The fixtures below are the real workflow-master layout, which contains the
 * exact trap this resolver exists to avoid: the CXADC clock-gen (the RF audio
 * path, not the dongle) sits on the SAME PCI controller one port away, at
 * usb-0000:00:14.0-13.1. Matching anything coarser than the full bus address
 * picks it and streams the wrong audio.
 */

#include "gui_alsa_device.h"

#include <stdio.h>
#include <string.h>

static int failures;

/* The real /proc/asound/cards from workflow-master. */
static const char *CARDS_REAL =
    " 0 [test_cg_audio  ]: USB-Audio - CXADC+ADC-ClockGen\n"
    "                      Rene Wolf CXADC+ADC-ClockGen at usb-0000:00:14.0-13.1, full speed\n"
    " 1 [PCH            ]: HDA-Intel - HDA Intel PCH\n"
    "                      HDA Intel PCH at 0x4012110000 irq 160\n"
    " 2 [NVidia         ]: HDA-Intel - HDA NVidia\n"
    "                      HDA NVidia at 0x95080000 irq 17\n"
    " 3 [MS210x         ]: USB-Audio - MS210x\n"
    "                      MacroSilicon MS210x at usb-0000:00:14.0-13.3, high speed\n";

/* Two identical dongles, as when a second capture station is plugged in. */
static const char *CARDS_TWO_DONGLES =
    " 0 [MS210x         ]: USB-Audio - MS210x\n"
    "                      MacroSilicon MS210x at usb-0000:00:14.0-13.3, high speed\n"
    " 1 [MS210x_1       ]: USB-Audio - MS210x\n"
    "                      MacroSilicon MS210x at usb-0000:00:14.0-2.1, high speed\n";

/* Same cards, different indices -- what a reboot or replug does. */
static const char *CARDS_REINDEXED =
    " 0 [MS210x         ]: USB-Audio - MS210x\n"
    "                      MacroSilicon MS210x at usb-0000:00:14.0-13.3, high speed\n"
    " 1 [test_cg_audio  ]: USB-Audio - CXADC+ADC-ClockGen\n"
    "                      Rene Wolf CXADC+ADC-ClockGen at usb-0000:00:14.0-13.1, full speed\n";

static int expect_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "ASSERTION FAILED: %s: expected=\"%s\" got=\"%s\"\n", what, want, got);
        failures++;
        return 1;
    }
    return 0;
}

static int expect_eq_int(int got, int want, const char *what)
{
    if (got != want) {
        fprintf(stderr, "ASSERTION FAILED: %s: expected=%d got=%d\n", what, want, got);
        failures++;
        return 1;
    }
    return 0;
}

static int test_resolves_the_dongle_by_bus_address(void)
{
    char out[128] = {0};
    int before = failures;

    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_REAL, "usb-0000:00:14.0-13.3",
                                              out, sizeof out), 0, "resolve");
    /* plughw, not hw: hw demands an exact format match and fails outright when
     * the dongle's native rate is not what we asked for. */
    expect_str(out, "plughw:CARD=MS210x,DEV=0", "resolved device");

    if (failures == before) puts("PASS: resolves the dongle by bus address");
    return failures - before;
}

/* The trap: same PCI controller, one port away, and it is the RF audio path.
 * Picking it would stream CXADC audio over the dongle's picture. */
static int test_never_picks_the_cxadc_card_on_the_same_controller(void)
{
    char out[128] = {0};
    int before = failures;

    gui_alsa_resolve_from_cards(CARDS_REAL, "usb-0000:00:14.0-13.3", out, sizeof out);
    if (strstr(out, "test_cg_audio") != NULL || strstr(out, "CXADC") != NULL) {
        fprintf(stderr, "ASSERTION FAILED: resolved the CXADC card: \"%s\"\n", out);
        failures++;
    }

    /* And asking for the CXADC's own address must resolve to the CXADC, not to
     * whichever USB card happened to come first. */
    char rf[128] = {0};
    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_REAL, "usb-0000:00:14.0-13.1",
                                              rf, sizeof rf), 0, "resolve cxadc");
    expect_str(rf, "plughw:CARD=test_cg_audio,DEV=0", "cxadc device");

    if (failures == before) puts("PASS: never picks the CXADC card on the same controller");
    return failures - before;
}

static int test_picks_the_right_one_of_two_identical_dongles(void)
{
    char a[128] = {0}, b[128] = {0};
    int before = failures;

    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_TWO_DONGLES, "usb-0000:00:14.0-13.3",
                                              a, sizeof a), 0, "resolve first");
    expect_str(a, "plughw:CARD=MS210x,DEV=0", "first dongle");

    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_TWO_DONGLES, "usb-0000:00:14.0-2.1",
                                              b, sizeof b), 0, "resolve second");
    expect_str(b, "plughw:CARD=MS210x_1,DEV=0", "second dongle");

    if (failures == before) puts("PASS: picks the right one of two identical dongles");
    return failures - before;
}

/* The reason indices are never stored: the same dongle is card 3 in one boot
 * and card 0 in the next, but its bus address does not move. */
static int test_survives_card_reindexing(void)
{
    char out[128] = {0};
    int before = failures;

    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_REINDEXED, "usb-0000:00:14.0-13.3",
                                              out, sizeof out), 0, "resolve after reindex");
    expect_str(out, "plughw:CARD=MS210x,DEV=0", "same device, different index");

    if (failures == before) puts("PASS: survives card reindexing");
    return failures - before;
}

static int test_reports_no_match_rather_than_guessing(void)
{
    char out[128] = {0};
    int before = failures;

    /* A dongle with no audio node, or one unplugged since enumeration. */
    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_REAL, "usb-0000:00:14.0-9.9",
                                              out, sizeof out), -1, "no matching card");
    expect_str(out, "", "output cleared on failure");

    /* Non-USB cards must never be offered as a fallback. */
    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_REAL, "", out, sizeof out), -1,
                  "empty bus info");
    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_REAL, NULL, out, sizeof out), -1,
                  "null bus info");
    expect_eq_int(gui_alsa_resolve_from_cards(NULL, "usb-0000:00:14.0-13.3", out, sizeof out),
                  -1, "null cards text");

    if (failures == before) puts("PASS: reports no match rather than guessing");
    return failures - before;
}

/* A partial bus address must not match a longer one: port 13.3 and port 13.30
 * are different devices, and "13.3" is a prefix of "13.30". */
static int test_does_not_match_a_bus_address_prefix(void)
{
    static const char *cards =
        " 0 [Other          ]: USB-Audio - Other\n"
        "                      Other at usb-0000:00:14.0-13.30, high speed\n";
    char out[128] = {0};
    int before = failures;

    expect_eq_int(gui_alsa_resolve_from_cards(cards, "usb-0000:00:14.0-13.3",
                                              out, sizeof out), -1, "prefix must not match");

    if (failures == before) puts("PASS: does not match a bus address prefix");
    return failures - before;
}

static int test_rejects_a_buffer_that_cannot_hold_the_device(void)
{
    char tiny[8] = {0};
    int before = failures;

    expect_eq_int(gui_alsa_resolve_from_cards(CARDS_REAL, "usb-0000:00:14.0-13.3",
                                              tiny, sizeof tiny), -1, "tiny buffer");

    if (failures == before) puts("PASS: rejects a buffer that cannot hold the device");
    return failures - before;
}

int main(void)
{
    test_resolves_the_dongle_by_bus_address();
    test_never_picks_the_cxadc_card_on_the_same_controller();
    test_picks_the_right_one_of_two_identical_dongles();
    test_survives_card_reindexing();
    test_reports_no_match_rather_than_guessing();
    test_does_not_match_a_bus_address_prefix();
    test_rejects_a_buffer_that_cannot_hold_the_device();

    if (failures != 0) {
        fprintf(stderr, "FAILED: %d alsa resolver assertion(s)\n", failures);
        return 1;
    }
    puts("PASS: alsa device resolve harness");
    return 0;
}
