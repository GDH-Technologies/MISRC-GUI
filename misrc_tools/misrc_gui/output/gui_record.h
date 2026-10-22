/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#ifndef GUI_RECORD_H
#define GUI_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gui_record_fifo.h"

// Forward declarations
typedef struct gui_app gui_app_t;

typedef enum {
    GUI_ERROR_CLASS_NONE = 0,
    GUI_ERROR_CLASS_SYSTEM = 1,
    GUI_ERROR_CLASS_PARSER = 2
} gui_error_class_t;

// Initialize recording subsystem
void gui_record_init(void);

// Cleanup recording subsystem
void gui_record_cleanup(void);

// Recording start return codes
#define RECORD_OK       0   // Recording started successfully
#define RECORD_ERROR   -1   // Error occurred
#define RECORD_PENDING  1   // Waiting for user confirmation (popup shown)

// Start recording to files
// Returns RECORD_OK on success, RECORD_ERROR on error, RECORD_PENDING if waiting for confirmation
int gui_record_start(gui_app_t *app);

// Check popup result and continue recording if confirmed
// Call this each frame when gui_record_is_pending() returns true
void gui_record_check_popup(gui_app_t *app);

// Check if waiting for popup confirmation
bool gui_record_is_pending(void);

// The overwrite prompt's message while gui_record_is_pending(), else "".
// The net server publishes it so a client can show the same prompt.
const char *gui_record_pending_message(void);

// Answer the pending overwrite prompt as if its button had been clicked
// (a net client's answer, applied on the server's main thread). The next
// gui_record_check_popup() then starts or cancels. No-op when nothing is
// pending.
void gui_record_resolve_pending(gui_app_t *app, bool confirm);

// Stop recording
void gui_record_stop(gui_app_t *app);

// Returns true while record-stop finalization is running in background.
bool gui_record_is_finalizing(void);

// Check if recording is active
bool gui_record_is_active(void);

// Check if any recording channel has a persistent output-file write error
// (e.g. the file is locked by another app). Used by the UI to flash the
// finalizing icon when capture ends while the write issue persists.
bool gui_record_has_write_error(void);

// Append a timestamped capture/record event to the active session log (if any)
void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t error_count);

// Record producers (the extraction thread). Each recorded block goes into
// the channel's ordered record FIFO (gui_record_fifo.h: the record ring
// first, a local spill file for overflow; every accepted byte reaches the
// writer once, in order). Bracket the pushes of one extraction iteration
// with enter/leave -- always leave, on every path -- so gui_record_stop can
// wait until no push is running before it closes the FIFOs:
//
//   bool on = gui_record_producer_enter();
//   if (on) { gui_record_push(0, a, len, frame); gui_record_push(1, b, len, frame); }
//   gui_record_producer_leave();
//
// push() never waits and never touches the output filesystem. It returns
// IDLE when the channel has no FIFO (not recording, or a direct channel fed
// by the CXADC tap), DROPPED when the block was lost (already logged with
// its RF sample position and counted; the caller decides stop_on_dropout).
bool gui_record_producer_enter(void);
void gui_record_producer_leave(void);
gui_record_push_result_t gui_record_push(int channel, const void *bytes, size_t len,
                                         uint32_t frame_index);

// Capture-buffer (BUF_CAPTURE_RF) blocks lost while recording: the
// extraction thread reports new drops with the RF position it had reached,
// so the gap is logged and counted against the recording.
void gui_record_note_capture_drops(gui_app_t *app, uint64_t new_drops, uint32_t frame_index);

// Direct native RAW passthrough (CXADC, FLAC off, per-channel resampling
// off): push the exact bytes just read from CXADC card `card` (0 = channel
// A, 1 = channel B) into that channel's record path. No-op unless the
// channel was set up as direct native at record start (decided by
// gui_record.c). Drop handling (capture log + stop_on_dropout) mirrors the
// extraction thread's record-drop path.
void gui_record_direct_tap_push(int card, const uint8_t *bytes, size_t len, uint32_t frame_index);

// Iteration-scoped tap gate for multi-channel lockstep capture threads
// (CXADC): latch the record decision once per iteration so a record
// start/stop handshake landing between the per-channel pushes cannot split
// the channels by one chunk. tap_channel_active() reports whether the
// channel is a direct native RAW channel this record session; enter/leave
// mark the channel's tap in-flight (covering the gated pushes for the
// stopper's disable + wait-idle handshake); tap_push_latched honors the
// iteration-latched decision.
bool gui_record_direct_tap_channel_active(int channel);
bool gui_record_tap_iteration_enter(int channel);
void gui_record_tap_iteration_leave(int channel);
void gui_record_direct_tap_push_latched(int channel, bool latched_on,
                                         const uint8_t *bytes, size_t len,
                                         uint32_t frame_index);

// Proactive low-disk guard for active recording paths.
// Returns true when free space drops below threshold and capture should be stopped safely.
bool gui_record_check_disk_space_guard(gui_app_t *app, uint32_t frame_index,
                                       char *status_msg, size_t status_msg_size);
// Query currently available free bytes for app->settings.output_path.
// Returns false when output_path is unset or the filesystem query fails.
bool gui_record_get_output_free_space_bytes(const gui_app_t *app, uint64_t *free_bytes_out);

/* Headless check that the two auto-name functions have not drifted. */
int gui_record_name_test_main(void);

int gui_record_video_settings_test_main(void);
int gui_record_cc_settings_test_main(void);

int gui_record_auto_record_main(const char *out_dir, int seconds, bool with_video,
                                bool use_flac, bool with_cc);

/* --capture-meta-selftest [keep_dir] [linked_seconds]: headless. A linked
 * FLAC A+B recording and an unlinked RAW one from the simulated device, then
 * the sidecar, the capture log's metadata block and the RF FLAC tags are
 * checked against what was linked. Uses a scratch settings file (never the
 * live one). Outputs go to TMPDIR/TEMP and are removed, unless keep_dir is
 * given (then they stay in keep_dir/{linked,unlinked}). Exit code 0 = pass. */
int gui_record_capture_meta_selftest_main(const char *keep_dir, int linked_seconds);

/* --record-gate-selftest: headless, no device and no files. With a finalize
 * marked in flight gui_record_start must refuse ("Finalizing previous
 * recording..."); with none it must get past that gate. Exit code 0 = pass. */
int gui_record_finalize_gate_selftest_main(void);

/* --record-order-selftest [keep_dir]: headless. The Simulated device emits
 * the record-order counter stream (gui_record_order_check.h), the RF writers
 * are stalled for 3 s so the record rings (1 GB budget) overflow, and a FLAC
 * and a converted RAW A+B recording are read back: every sample must be
 * present once, in capture order, and the capture log must show the
 * overflow. Outputs go to TMPDIR/TEMP and are removed unless keep_dir is
 * given. Exit code 0 = pass, 1 = fail, 2 = could not run. */
int gui_record_order_selftest_main(const char *keep_dir);

/* Debug: stall the RF writer threads from start_ms after the record start
 * for duration_ms (repeating every period_ms when > 0); start_ms < 0 turns it
 * off. MISRC_GUI_DEBUG_RECORD_WRITER_STALL="start:duration[:period]" sets it
 * at the next record start. */
void gui_record_debug_set_writer_stall(int start_ms, int duration_ms, int period_ms);

// Live on-disk size of each channel's output file via stat(). For the UI
// readout so it shows the exact file size (1:1 with `ls`/file-manager), not
// the writer-thread atomic which can lead the flushed file by the FILE*
// buffer. Returns false when not recording; per-channel 0 if that file
// isn't open or stat fails.
bool gui_record_get_live_output_bytes(uint64_t *out_a, uint64_t *out_b);

#endif // GUI_RECORD_H
