"""Compile the actual native UI timeline helpers against small playback fixtures.

This exercises loaded/unknown/provisional/confirmed transitions and seek gating without starting
raylib, scanning a media file, or copying the production helper implementation.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def extract_function(source: str, name: str) -> str:
    match = re.search(r"static\s+(?:void|bool)\s+" + re.escape(name) + r"\s*\(", source)
    if not match:
        raise RuntimeError(f"Production helper missing: {name}")
    opening = source.index("{", match.start())
    depth = 0
    state = "code"
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line":
            if char == "\n":
                state = "code"
        elif state == "comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if char == "\\":
                index += 1
            elif char == ('"' if state == "string" else "'"):
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line"
            index += 1
        elif char == "/" and next_char == "*":
            state = "comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():index + 1]
        index += 1
    raise RuntimeError(f"Unterminated production helper: {name}")


FIXTURES = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "gui_playback.h"

struct gui_app { bool running; };
typedef struct {
    bool found;
    struct { float x, width; } boundingBox;
} Clay_ElementData;
#define CLAY_IDI(name, index) (index)

static playback_file_info_t files[2];
static playback_file_info_t selected_files[2];
static bool selection_present[2];
static double rates[2];
static Clay_ElementData tracks[2] = {
    {true, {10.0f, 100.0f}}, {true, {10.0f, 100.0f}}
};
static unsigned seek_calls;
static int sought_channel;
static uint64_t sought_sample;

bool gui_playback_get_file_info_a(gui_app_t *app, playback_file_info_t *info) {
    (void)app; *info = files[0]; return info->valid;
}
bool gui_playback_get_file_info_b(gui_app_t *app, playback_file_info_t *info) {
    (void)app; *info = files[1]; return info->valid;
}
bool gui_playback_get_selected_file_info(gui_app_t *app, int channel, playback_file_info_t *info) {
    if (!app || !info) return false;
    memset(info, 0, sizeof(*info));
    if (channel != 0 && channel != 1) return false;
    if (app->running && files[channel].valid) {
        *info = files[channel];
        return true;
    }
    if (!selection_present[channel]) return false;
    /* Validation may return header details for an invalid selected file;
       only info.valid makes it a usable timeline. */
    *info = selected_files[channel];
    return info->valid;
}
double gui_playback_get_channel_rate_hz(gui_app_t *app, int channel) {
    (void)app; return rates[channel];
}
static Clay_ElementData Clay_GetElementData(int channel) { return tracks[channel]; }
void gui_playback_seek_sample_channel(gui_app_t *app, int channel, uint64_t sample) {
    (void)app; ++seek_calls; sought_channel = channel; sought_sample = sample;
}
'''

CHECKS = r'''
int main(void) {
    gui_app_t app = {0};
    uint64_t total = 999;
    double duration = 999;
    char text[64];
    int fill = 999;
    bool loaded = true;

    assert(!gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration, NULL, NULL));
    assert(total == 0 && duration == 0);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        false, false, 0, 0, 0, 0, 100);
    assert(!loaded && fill == 0 && strcmp(text, "--:--:--/--:--:--") == 0);

    app.running = true;
    files[0].valid = true;
    for (unsigned rate_mhz = 20; rate_mhz <= 80; rate_mhz *= 2) {
        rates[0] = rate_mhz * 1000000.0;
        assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration, NULL, NULL));
        assert(total == 0 && duration == 0);
        gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
            true, false, (uint64_t)(rates[0] * 12), total, duration, rates[0], 100);
        assert(loaded && fill == 0 && strcmp(text, "00:00:12/Unknown") == 0);
        assert(!gui_ui_seek_playback_from_track(&app, 0, 60.0f));
        assert(seek_calls == 0);
    }

    /* B may have a usable length while A is still being indexed. */
    rates[1] = 20000000.0;
    files[1] = (playback_file_info_t){.valid = true, .total_samples = 240000000};
    assert(gui_ui_playback_channel_timeline_info(&app, 1, &total, &duration, NULL, NULL));
    assert(total == 240000000 && duration == 12.0);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, false, 40000000, total, duration, rates[1], 300);
    assert(loaded && fill == 50 && strcmp(text, "00:00:02/00:00:12") == 0);
    assert(gui_ui_seek_playback_from_track(&app, 1, 60.0f));
    assert(seek_calls == 1 && sought_channel == 1 && sought_sample == 120000000);
    assert(gui_ui_seek_playback_from_track(&app, 1, 500.0f));
    assert(sought_sample == 239999999);
    assert(!gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    assert(seek_calls == 2);

    /* Publishing A's length is sufficient; no UI reload or cached state needed. */
    rates[0] = 40000000.0;
    files[0].total_samples = 800000000;
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration, NULL, NULL));
    assert(total == 800000000 && duration == 20.0);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, false, 480000000, total, duration, rates[0], 100);
    assert(loaded && fill == 60 && strcmp(text, "00:00:12/00:00:20") == 0);
    assert(gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    assert(seek_calls == 3 && sought_channel == 0 && sought_sample == 400000000);

    /* Loop Off can reach EOF before indexing finishes. Publishing the total
       must retain the end position, rather than wrap to the beginning. */
    files[0].total_samples = 0;
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration, NULL, NULL));
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, false, 800000000, total, duration, rates[0], 100);
    assert(loaded && fill == 0 && strcmp(text, "00:00:20/Unknown") == 0);
    assert(!gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    files[0].total_samples = 800000000;
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration, NULL, NULL));
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, false, 800000000, total, duration, rates[0], 100);
    assert(loaded && fill == 100 && strcmp(text, "00:00:20/00:00:20") == 0);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, false, 800000001, total, duration, rates[0], 100);
    assert(fill == 100 && strcmp(text, "00:00:20/00:00:20") == 0);
    assert(seek_calls == 3);
    /* Scan progress is independent from playback time and never enables seek. */
    char scan_text[96];
    playback_index_progress_t progress = {0};
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, false);
    assert(scan_text[0] == '\0');
    progress.active = true;
    progress.queued = true;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, false);
    assert(strcmp(scan_text, "Length scan queued") == 0);
    progress.queued = false;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, false);
    assert(strcmp(scan_text, "Starting length scan...") == 0);
    progress.total_bytes = 71470217481ULL;
    progress.bytes_scanned = 35735108740ULL;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, false);
    assert(strcmp(scan_text, "Scanning length: 49% (35.7/71.5 GB)") == 0);
    progress.waiting = true;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, true, false);
    assert(strcmp(scan_text, "Scan waiting: 49%") == 0);
    progress.bytes_scanned = UINT64_MAX;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, false);
    assert(strcmp(scan_text, "Scan waiting: 100% (71.5/71.5 GB)") == 0);
    progress.total_bytes = UINT64_MAX;
    progress.waiting = false;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, true, false);
    assert(strcmp(scan_text, "Scanning length: 100%") == 0);
    files[0].total_samples = 0;
    assert(!gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    assert(seek_calls == 3);
    /* Tail-derived totals are visibly provisional and immediately seekable.
       An underestimated total never hides actual elapsed playback time. */
    bool provisional = false, failed = true;
    files[0] = (playback_file_info_t){.valid = true, .total_samples = 800000000,
        .length_provisional = true};
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration,
                                                &provisional, &failed));
    assert(provisional && !failed && duration == 20.0);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, provisional, 480000000, total, duration, rates[0], 100);
    assert(fill == 60 && strcmp(text, "00:00:12/~00:00:20") == 0);
    assert(gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    assert(seek_calls == 4 && sought_sample == 400000000);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, provisional, 1200000000, total, duration, rates[0], 100);
    assert(fill == 100 && strcmp(text, "00:00:30/~00:00:20") == 0);
    /* Background confirmation adjusts the scale without storing a playhead. */
    files[0].total_samples = 1600000000;
    files[0].length_provisional = false;
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration,
                                                &provisional, &failed));
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, provisional, 1200000000, total, duration, rates[0], 100);
    assert(!provisional && fill == 75 && strcmp(text, "00:00:30/00:00:40") == 0);
    files[0].total_samples = 0;
    files[0].length_verification_failed = true;
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration,
                                                &provisional, &failed));
    assert(!provisional && failed && total == 0 && duration == 0);
    assert(!gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    assert(seek_calls == 4);
    progress.queued = true;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, true);
    assert(strcmp(scan_text, "Verification queued") == 0);
    progress.queued = false;
    progress.total_bytes = 0;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, true);
    assert(strcmp(scan_text, "Starting verification...") == 0);
    progress.total_bytes = 71470217481ULL;
    progress.bytes_scanned = 35735108740ULL;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, false, true);
    assert(strcmp(scan_text, "Verifying length: 49% (35.7/71.5 GB)") == 0);
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, true, true);
    assert(strcmp(scan_text, "Verifying: 49%") == 0);
    progress.waiting = true;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, true, true);
    assert(strcmp(scan_text, "Verification waiting: 49%") == 0);
    progress.active = false;
    gui_ui_format_playback_index_progress(scan_text, sizeof(scan_text), &progress, true, true);
    assert(scan_text[0] == '\0');
    /* At idle, the selected file is authoritative rather than stale live
       metadata retained from the previous session. */
    app.running = false;
    files[0] = (playback_file_info_t){.valid = true, .total_samples = 800000000,
        .duration_seconds = 20.0, .real_sample_rate_hz = 40000000.0};
    selected_files[0] = (playback_file_info_t){.valid = true, .total_samples = 100000000,
        .duration_seconds = 5.0, .real_sample_rate_hz = 20000000.0};
    selection_present[0] = true;
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration,
                                                &provisional, &failed));
    assert(total == 100000000 && duration == 5.0 && !provisional && !failed);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, false, 0, total, duration, selected_files[0].real_sample_rate_hz, 100);
    assert(loaded && fill == 0 && strcmp(text, "00:00:00/00:00:05") == 0);

    /* No selected B file must not resurrect its valid previous-session info. */
    assert(files[1].valid && !selection_present[1]);
    assert(!gui_ui_playback_channel_timeline_info(&app, 1, &total, &duration,
                                                 &provisional, &failed));
    assert(total == 0 && duration == 0 && !provisional && !failed);
    assert(!gui_ui_seek_playback_from_track(&app, 1, 60.0f));

    /* An invalid selection can expose bit depth/header diagnostics but must
       remain distinct from a valid file whose length is not known yet. */
    selected_files[0] = (playback_file_info_t){.bits_per_sample = 16,
        .total_samples = 100000000, .duration_seconds = 5.0};
    playback_file_info_t selected = {0};
    assert(!gui_playback_get_selected_file_info(&app, 0, &selected));
    assert(!selected.valid && selected.bits_per_sample == 16);
    assert(!gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration,
                                                 &provisional, &failed));
    assert(total == 0 && duration == 0 && !provisional && !failed);
    assert(!gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    selected_files[0] = (playback_file_info_t){.valid = true,
        .real_sample_rate_hz = 20000000.0};
    assert(gui_ui_playback_channel_timeline_info(&app, 0, &total, &duration,
                                                &provisional, &failed));
    assert(total == 0 && duration == 0 && !provisional && !failed);
    gui_ui_format_playback_timeline(text, sizeof(text), &fill, &loaded,
        true, false, 0, total, duration, selected_files[0].real_sample_rate_hz, 100);
    assert(loaded && fill == 0 && strcmp(text, "00:00:00/Unknown") == 0);
    assert(!gui_ui_seek_playback_from_track(&app, 0, 60.0f));
    assert(seek_calls == 4);

    assert(!gui_ui_playback_channel_timeline_info(&app, 2, &total, &duration,
                                                 &provisional, &failed));
    assert(total == 0 && duration == 0 && !provisional && !failed);
    assert(!gui_ui_playback_channel_timeline_info(NULL, 0, &total, &duration, NULL, NULL));
    assert(total == 0 && duration == 0);
    puts("UI timeline: unknown elapsed at 20/40/80 MHz; independent A/B; provisional seek and correction; failed verification withdrawal; discovery/verification progress; EOF recovery; idle selected-file and invalid-selection distinctions passed");
    return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("gcc") or shutil.which("cc"))
    parser.add_argument("--temp-dir", type=Path, help="Directory for temporary compiled fixtures")
    args = parser.parse_args()
    if not args.cc:
        parser.error("A C compiler is required; pass --cc")
    env = os.environ.copy()
    compiler = shutil.which(args.cc) or args.cc
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
    source_path = Path(__file__).resolve().parents[1] / "misrc_gui/ui/gui_ui.c"
    source = source_path.read_text(encoding="utf-8")
    helpers = "\n\n".join(extract_function(source, name) for name in (
        "format_playback_timecode",
        "gui_ui_playback_channel_timeline_info",
        "gui_ui_format_playback_timeline",
        "gui_ui_seek_playback_from_track",
        "gui_ui_format_playback_index_progress",
    ))
    with tempfile.TemporaryDirectory(prefix="misrc-ui-timeline-", dir=args.temp_dir) as directory:
        test_source = Path(directory) / "timeline.c"
        executable = Path(directory) / "timeline.exe"
        test_source.write_bytes((FIXTURES + helpers + CHECKS).replace("\n", "\r\n").encode())
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wshadow",
                        "-I", str(source_path.parents[1] / "input"),
                        "-Werror", str(test_source), "-lm", "-o", str(executable)], check=True, env=env)
        subprocess.run([str(executable)], check=True, timeout=10, env=env)


if __name__ == "__main__":
    main()
