/*
 * Guard harness: the closed-caption ffmpeg command line.
 *
 * The invariants asserted here are all ones whose violation is SILENT -- the
 * capture still runs, the file still appears, and the damage is only visible
 * much later:
 *
 *   - two -i tokens would mean two opens of an EXCLUSIVE device; the second
 *     fails with EBUSY and the recording quietly loses its captions.
 *   - -raw_timestamps would leave the sidecar on kernel-clock timestamps
 *     instead of rebased-to-zero, so it would not line up with a recording
 *     that starts at t=0. It is the option someone reaches for when trying to
 *     "fix" the one-frame offset, and nothing would report the damage.
 *   - a missing -y makes ffmpeg block on its own interactive overwrite
 *     question, with the GUI holding a record session open behind it.
 *   - a missing -nostdin lets the child eat the terminal's input.
 *
 * This links gui_cc_record.c and NOTHING else from the project, which is only
 * possible because that module has no project includes. Keep it that way.
 */

#include "gui_cc_record.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *what)
{
    printf("FAIL: %s\n", what);
    failures++;
}

static int find_tok(const char *argv[], int n, const char *tok)
{
    for (int i = 0; i < n; i++)
        if (strcmp(argv[i], tok) == 0) return i;
    return -1;
}

static int count_tok(const char *argv[], int n, const char *tok)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(argv[i], tok) == 0) c++;
    return c;
}

int main(void)
{
    const char *dev = "/dev/vbi3";
    const char *out = "/tmp/guard/session_captions.scc";

    char buf[2048];
    const char *argv[48];
    int n = gui_cc_record_build_argv_test(dev, out, buf, sizeof(buf), argv, 48);

    if (n <= 0) {
        printf("FAIL: build_argv returned %d\n", n);
        return 1;
    }
    if (argv[n] != NULL) fail("argv is not NULL-terminated");

    /* One input, and it is the device. */
    if (count_tok(argv, n, "-i") != 1) fail("expected exactly one -i");
    int i_at = find_tok(argv, n, "-i");
    if (i_at < 0 || i_at + 1 >= n || strcmp(argv[i_at + 1], dev) != 0)
        fail("-i is not followed by the device path");

    /* The input format must be declared BEFORE -i, or ffmpeg probes the node
     * as a regular file and fails. Demuxer options (-fill_nulls) legitimately
     * sit between the two, so this is an ordering test, not an adjacency one:
     * ffmpeg binds every option to the next -i that follows it, which is
     * exactly why an input option that drifted after -i would silently become
     * an OUTPUT option instead. */
    int f_at = find_tok(argv, n, "-f");
    if (f_at < 0 || f_at + 1 >= n || strcmp(argv[f_at + 1], "v4l2vbi") != 0)
        fail("the first -f does not declare v4l2vbi");
    if (f_at > i_at) fail("-f v4l2vbi comes after -i, so it would not apply to the input");

    /* Exactly one output, and it is the .scc we were asked for. */
    if (strcmp(argv[n - 1], out) != 0) fail("the last token is not the output path");
    size_t olen = strlen(argv[n - 1]);
    if (olen < 4 || strcmp(argv[n - 1] + olen - 4, ".scc") != 0)
        fail("the output path does not end in .scc");

    /* Byte-exact copy: transcoding through the decoder would discard XDS and
     * any field-2 data the driver later learns to slice. */
    int c_at = find_tok(argv, n, "-c:s");
    if (c_at < 0 || c_at + 1 >= n || strcmp(argv[c_at + 1], "copy") != 0)
        fail("-c:s is not 'copy'");
    if (count_tok(argv, n, "-c:s") != 1) fail("expected exactly one -c:s");

    int m_at = find_tok(argv, n, "-map");
    if (m_at < 0 || m_at + 1 >= n || strcmp(argv[m_at + 1], "0:s:0") != 0)
        fail("-map is not '0:s:0'");

    /* The muxer is named explicitly rather than inferred from the extension. */
    int muxer_ok = 0;
    for (int i = 0; i + 1 < n; i++)
        if (strcmp(argv[i], "-f") == 0 && strcmp(argv[i + 1], "scc") == 0) muxer_ok = 1;
    if (!muxer_ok) fail("output muxer is not explicitly -f scc");

    /* Settled by measurement: the SCC muxer drops null pairs, so this changes
     * nothing here -- but it is the device default and stays correct if the
     * stream is ever muxed against video. Asserted so it cannot drift silently. */
    int fn_at = find_tok(argv, n, "-fill_nulls");
    if (fn_at < 0 || fn_at + 1 >= n || strcmp(argv[fn_at + 1], "1") != 0)
        fail("-fill_nulls is not explicitly 1");
    /* It is a DEMUXER option: after -i it would be parsed as an output option
     * against a muxer that has never heard of it, and ffmpeg would refuse to
     * start -- at record time, not here. */
    if (fn_at > i_at) fail("-fill_nulls comes after -i, where it is not an input option");

    int fp_at = find_tok(argv, n, "-flush_packets");
    if (fp_at < 0 || fp_at + 1 >= n || strcmp(argv[fp_at + 1], "1") != 0)
        fail("-flush_packets is not explicitly 1 (a killed child would lose the tail)");

    if (find_tok(argv, n, "-raw_timestamps") >= 0)
        fail("-raw_timestamps must never be passed: it unrebases the sidecar from t=0");
    if (find_tok(argv, n, "-copyts") >= 0)
        fail("-copyts must never be passed");

    if (find_tok(argv, n, "-nostdin") < 0) fail("-nostdin is missing");
    if (find_tok(argv, n, "-y") < 0)       fail("-y is missing (ffmpeg would block on overwrite)");
    if (find_tok(argv, n, "-hide_banner") < 0) fail("-hide_banner is missing");

    /* Refusals: a caller that lost its device or path must not produce a
     * command line that would run against something arbitrary. */
    if (gui_cc_record_build_argv_test("", out, buf, sizeof(buf), argv, 48) >= 0)
        fail("an empty device was accepted");
    if (gui_cc_record_build_argv_test(dev, "", buf, sizeof(buf), argv, 48) >= 0)
        fail("an empty output path was accepted");
    if (gui_cc_record_build_argv_test(dev, out, buf, 8, argv, 48) >= 0)
        fail("a too-small token buffer was accepted");
    if (gui_cc_record_build_argv_test(dev, out, buf, sizeof(buf), argv, 4) >= 0)
        fail("a too-small argv array was accepted");

    if (failures) {
        printf("cc argv harness FAILED (%d)\n", failures);
        return 1;
    }
    printf("cc argv harness passed (%d tokens)\n", n);
    return 0;
}
