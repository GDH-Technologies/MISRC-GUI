/*
 * CPU-only regression tests for the playback file-info resolution and the
 * decoder IO layer (STREAMINFO patch). Include the implementation to
 * exercise its private probe/IO helpers. No GUI, window or capture backend
 * is needed; framework symbols are stubbed.
 *
 * Two on-disk FLAC header conventions are synthesized with libFLAC and must
 * both resolve to the TRUE stream sample count and real RF rate:
 *   OLD (pre-finalize): STREAMINFO total = raw sample count, header rate is
 *                       a kHz-domain value (20000 = 20 MSPS).
 *   NEW (finalized):    STREAMINFO total = samples/1000 (kHz domain, so the
 *                       header duration is truthful) + RF vorbis tags.
 * The NEW convention previously made libFLAC reject every seek past the
 * header total; the IO layer must patch the decoder-visible STREAMINFO so
 * seeking works across the real stream.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "FLAC/stream_encoder.h"
#include "FLAC/metadata.h"

#include "../misrc_gui/input/gui_playback.c"

// ---- Framework stubs (not under test) ----
void gui_app_set_status(gui_app_t *app, const char *message) { (void)app; (void)message; }
void gui_extract_prepare_stream(gui_app_t *app) { (void)app; }
int gui_extract_start_tagged(gui_app_t *app) { (void)app; return 0; }
bool gui_extract_stamp_playback(const gui_rf_block_info_t info[2], const size_t counts[2])
{ (void)info; (void)counts; return true; }
void gui_extract_stop(void) {}
int gui_display_thread_start(display_thread_t *dt,
                             struct gui_app *app,
                             struct buffer_manager *bufmgr)
{ (void)dt; (void)app; (void)bufmgr; return 0; }
void gui_display_thread_stop(display_thread_t *dt) { (void)dt; }
void bufmgr_reset_stats(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; }
void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id,
                          size_t bytes, const backpressure_policy_t *policy)
{ (void)id; (void)bytes; (void)policy; return mgr ? NULL : NULL; }
void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes)
{ (void)mgr; (void)id; (void)bytes; }
void bufmgr_signal_data(buffer_manager_t *mgr, buffer_id_t id)
{ (void)mgr; (void)id; }
double GetTime(void) { return 0.0; }  /* raylib, not linked */

static int failures;
static int checks;
static const char *test_case;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

static void expect_u64(uint64_t got, uint64_t want, const char *message)
{
    checks++;
    if (got != want) {
        fprintf(stderr, "FAIL [%s]: %s (got %llu, want %llu)\n",
                test_case, message,
                (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void expect_near(double got, double want, const char *message)
{
    checks++;
    if (!(fabs(got - want) < 1.0e-9)) {
        fprintf(stderr, "FAIL [%s]: %s (got %.9f, want %.9f)\n",
                test_case, message, got, want);
        failures++;
    }
}

// ---- Synthetic capture generation ----

#define TEST_SAMPLE_RATE_KHZ 20000u   /* kHz-domain header rate: 20 MSPS RF */
#define TEST_RF_RATE_HZ 20000000.0
#define TEST_SAMPLES 3000000u         /* 0.15 s at 20 MSPS; NEW total = 3000 */

static bool write_test_flac(const char *path, uint32_t num_samples)
{
    FILE *f = fopen(path, "w+b");
    if (!f) return false;

    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) { fclose(f); return false; }

    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata *pad = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
    if (!vc || !pad) {
        if (vc) FLAC__metadata_object_delete(vc);
        if (pad) FLAC__metadata_object_delete(pad);
        FLAC__stream_encoder_delete(enc);
        fclose(f);
        return false;
    }
    pad->length = 4096;
    FLAC__StreamMetadata *meta[2] = { vc, pad };

    bool ok = FLAC__stream_encoder_set_channels(enc, 1) &&
              FLAC__stream_encoder_set_bits_per_sample(enc, 8) &&
              FLAC__stream_encoder_set_sample_rate(enc, TEST_SAMPLE_RATE_KHZ) &&
              FLAC__stream_encoder_set_compression_level(enc, 0) &&
              FLAC__stream_encoder_set_verify(enc, false) &&
              FLAC__stream_encoder_set_total_samples_estimate(enc, 0) &&
              FLAC__stream_encoder_set_metadata(enc, meta, 2);
    bool encoder_owns_file = false;
    if (ok) {
        ok = FLAC__stream_encoder_init_FILE(enc, f, NULL, NULL) ==
             FLAC__STREAM_ENCODER_INIT_STATUS_OK;
        /* After a successful init_FILE the encoder owns the FILE* and closes
         * it in finish()/delete(). */
        encoder_owns_file = ok;
    }
    if (ok) {
        /* Deterministic 8-bit signed noise-ish pattern. */
        FLAC__int32 buffer[4096];
        uint32_t remaining = num_samples;
        uint32_t seed = 12345;
        while (remaining > 0 && ok) {
            uint32_t block = (remaining > 4096) ? 4096 : remaining;
            for (uint32_t i = 0; i < block; i++) {
                seed = seed * 1103515245u + 12345u;
                buffer[i] = (FLAC__int32)((seed >> 16) & 0xFFu) - 128;
            }
            ok = FLAC__stream_encoder_process_interleaved(enc, buffer, block);
            remaining -= block;
        }
        FLAC__stream_encoder_finish(enc);
        ok = ok && (FLAC__stream_encoder_get_state(enc) == FLAC__STREAM_ENCODER_UNINITIALIZED);
    }

    FLAC__metadata_object_delete(vc);
    FLAC__metadata_object_delete(pad);
    FLAC__stream_encoder_delete(enc);
    if (!encoder_owns_file) {
        fclose(f);
    }
    return ok;
}

static bool append_tag(FLAC__StreamMetadata *block, const char *name, const char *value)
{
    FLAC__StreamMetadata_VorbisComment_Entry entry;
    if (!FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, name, value)) {
        return false;
    }
    if (!FLAC__metadata_object_vorbiscomment_append_comment(block, entry, /*copy=*/false)) {
        free(entry.entry);
        return false;
    }
    return true;
}

/* Mimic the recorder finalize: scale STREAMINFO total to the kHz domain and
 * embed the RF truth in vorbis tags. */
static bool finalize_test_flac(const char *path, uint32_t num_samples)
{
    FLAC__Metadata_SimpleIterator *it = FLAC__metadata_simple_iterator_new();
    if (!it) return false;
    if (!FLAC__metadata_simple_iterator_init(it, path, /*read_only=*/false,
                                             /*preserve_file_stats=*/true)) {
        FLAC__metadata_simple_iterator_delete(it);
        return false;
    }

    bool ok = false;
    do {
        FLAC__StreamMetadata *si = FLAC__metadata_simple_iterator_get_block(it);
        if (!si || si->type != FLAC__METADATA_TYPE_STREAMINFO) {
            if (si) FLAC__metadata_object_delete(si);
            break;
        }
        si->data.stream_info.total_samples = ((uint64_t)num_samples + 500u) / 1000u;
        if (si->data.stream_info.total_samples == 0) si->data.stream_info.total_samples = 1;
        ok = FLAC__metadata_simple_iterator_set_block(it, si, /*use_padding=*/false);
        FLAC__metadata_object_delete(si);
        if (!ok) break;

        while (FLAC__metadata_simple_iterator_next(it)) {
            FLAC__StreamMetadata *blk = FLAC__metadata_simple_iterator_get_block(it);
            if (!blk) continue;
            if (blk->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
                char total_str[32];
                char rate_str[32];
                char rate_khz_str[32];
                char duration_str[64];
                char length_str[32];
                snprintf(total_str, sizeof(total_str), "%u", (unsigned)num_samples);
                snprintf(rate_str, sizeof(rate_str), "%u", (unsigned)TEST_RF_RATE_HZ);
                snprintf(rate_khz_str, sizeof(rate_khz_str), "%u", TEST_SAMPLE_RATE_KHZ);
                snprintf(duration_str, sizeof(duration_str), "%.6f",
                         (double)num_samples / TEST_RF_RATE_HZ);
                snprintf(length_str, sizeof(length_str), "%u",
                         (unsigned)((double)num_samples / TEST_RF_RATE_HZ * 1000.0));
                ok = append_tag(blk, "RF_TOTAL_SAMPLES", total_str) &&
                     append_tag(blk, "RF_SAMPLE_RATE", rate_str) &&
                     append_tag(blk, "RF_SAMPLE_RATE_KHZ", rate_khz_str) &&
                     append_tag(blk, "DURATION_SECONDS", duration_str) &&
                     append_tag(blk, "LENGTH", length_str);
                if (ok) {
                    ok = FLAC__metadata_simple_iterator_set_block(it, blk, /*use_padding=*/true);
                }
                FLAC__metadata_object_delete(blk);
                break;
            }
            FLAC__metadata_object_delete(blk);
        }
    } while (0);

    FLAC__metadata_simple_iterator_delete(it);
    return ok;
}

// ---- IO-layer decoder harness (STREAMINFO patch + seek) ----

static uint64_t io_test_seen_total;

static FLAC__StreamDecoderWriteStatus io_test_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data)
{
    (void)decoder; (void)frame; (void)buffer; (void)client_data;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void io_test_meta_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
    (void)decoder; (void)client_data;
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        io_test_seen_total = metadata->data.stream_info.total_samples;
    }
}

static void io_test_error_cb(
    const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    (void)decoder; (void)client_data;
    fprintf(stderr, "harness decoder error: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

/* Open the finalized-convention file through the production IO layer with the
 * patch armed and verify the decoder sees the TRUE total and can seek past
 * the (lying) header total. */
static void test_io_layer_patch_and_seek(const char *path)
{
    test_case = "io_layer_patch_and_seek";

    playback_file_info_t info = {0};
    bool have_info = gui_playback_validate_file(path, &info);
    expect_true(have_info, "validation must read the header and RF tags");
    expect_u64(info.total_samples, TEST_SAMPLES, "resolved true total");
    expect_u64(info.header_total_samples, ((uint64_t)TEST_SAMPLES + 500u) / 1000u,
               "header total is the kHz-domain value");

    FILE *f = fopen(path, "rb");
    expect_true(f != NULL, "reopen file for IO layer test");
    if (!f) return;

    playback_io_t io;
    gui_playback_io_reset(&io, f, NULL, info.total_samples);
    expect_true(io.stage == PLAYBACK_IO_STAGE_SERVING, "patch staging armed");

    FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
    expect_true(dec != NULL, "decoder allocated");
    if (!dec) { fclose(f); return; }
    FLAC__stream_decoder_set_md5_checking(dec, false);
    FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_stream(
        dec, gui_playback_io_read_cb, gui_playback_io_seek_cb, gui_playback_io_tell_cb,
        gui_playback_io_length_cb, gui_playback_io_eof_cb,
        io_test_write_cb, io_test_meta_cb, io_test_error_cb, &io);
    expect_true(st == FLAC__STREAM_DECODER_INIT_STATUS_OK, "init_stream via IO layer");
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(dec);
        fclose(f);
        return;
    }

    expect_true(FLAC__stream_decoder_process_until_end_of_metadata(dec),
                "metadata pass through patched IO layer");
    expect_u64(io_test_seen_total, TEST_SAMPLES,
               "decoder-visible STREAMINFO total must be the patched true total");

    /* Without the patch these seeks fail: target >= header total (3000). */
    expect_true(FLAC__stream_decoder_seek_absolute(dec, TEST_SAMPLES / 2),
                "seek to mid-file (past header total) must succeed");
    expect_true(FLAC__stream_decoder_process_single(dec),
                "decode after mid-file seek");
    expect_true(FLAC__stream_decoder_seek_absolute(dec, TEST_SAMPLES - 10),
                "seek near end of real stream must succeed");
    expect_true(FLAC__stream_decoder_process_single(dec),
                "decode after end seek");
    expect_true(FLAC__stream_decoder_seek_absolute(dec, 0),
                "seek back to start must succeed");
    expect_true(FLAC__stream_decoder_process_single(dec),
                "decode after seek to start");

    FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    fclose(f);
}

int main(void)
{
    char path_old[256];
    char path_new[256];
    const char *temp_dir = getenv("TMPDIR");
#if defined(_WIN32) || defined(_WIN64)
    if (!temp_dir || !temp_dir[0]) temp_dir = getenv("TEMP");
    if (!temp_dir || !temp_dir[0]) temp_dir = getenv("TMP");
    if (!temp_dir || !temp_dir[0]) temp_dir = ".";
#else
    if (!temp_dir || !temp_dir[0]) temp_dir = "/tmp";
#endif
    int old_len = snprintf(path_old, sizeof(path_old),
                           "%s/misrc_pb_%d_test_rf_8-bit_20msps.flac",
                           temp_dir, (int)getpid());
    int new_len = snprintf(path_new, sizeof(path_new),
                           "%s/misrc_pb_%d_test_fin_8-bit_20msps.flac",
                           temp_dir, (int)getpid());
    if (old_len < 0 || (size_t)old_len >= sizeof(path_old) ||
        new_len < 0 || (size_t)new_len >= sizeof(path_new)) {
        fprintf(stderr, "temporary fixture path is too long\n");
        return 1;
    }

    test_case = "generate";
    expect_true(write_test_flac(path_old, TEST_SAMPLES), "write OLD-convention flac");
    expect_true(write_test_flac(path_new, TEST_SAMPLES), "write NEW-convention base flac");
    expect_true(finalize_test_flac(path_new, TEST_SAMPLES), "finalize NEW-convention flac");

    playback_file_info_t info;

    test_case = "resolve_old_convention";
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path_old, &info), "OLD file validates");
    expect_u64(info.header_total_samples, TEST_SAMPLES, "OLD header total is raw count");
    expect_u64(info.total_samples, TEST_SAMPLES, "OLD resolved total is the raw count");
    expect_u64((uint64_t)info.real_sample_rate_hz, (uint64_t)TEST_RF_RATE_HZ,
               "OLD resolved real rate (msps filename hint)");
    expect_near(info.duration_seconds, (double)TEST_SAMPLES / TEST_RF_RATE_HZ,
                "OLD true duration");
    expect_true(info.bits_per_sample == 8, "OLD bit depth");

    test_case = "resolve_new_convention";
    memset(&info, 0, sizeof(info));
    expect_true(gui_playback_validate_file(path_new, &info), "NEW file validates");
    expect_u64(info.header_total_samples, ((uint64_t)TEST_SAMPLES + 500u) / 1000u,
               "NEW header total is kHz-domain");
    expect_u64(info.total_samples, TEST_SAMPLES,
               "NEW resolved total must be the TRUE stream count, not the header value");
    expect_u64((uint64_t)info.real_sample_rate_hz, (uint64_t)TEST_RF_RATE_HZ,
               "NEW resolved real rate (RF tag)");
    expect_near(info.duration_seconds, (double)TEST_SAMPLES / TEST_RF_RATE_HZ,
                "NEW true duration");

    test_io_layer_patch_and_seek(path_new);

    unlink(path_old);
    unlink(path_new);

    printf("gui_playback_resolve: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
