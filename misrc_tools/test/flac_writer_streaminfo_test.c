/*
 * Runtime guard for the FLAC STREAMINFO total_samples contract.
 *
 * RF FLAC files store sample_rate in kHz (FLAC's 20-bit rate field cannot hold
 * 40 MHz), but STREAMINFO.total_samples must hold the true Hz-domain sample
 * count: libsndfile-based readers (vhs-decode's hifi-decode) trust it and stop
 * reading there, so any scaled or wrapped count silently truncates decodes.
 * This harness asserts:
 *   1. flac_writer encodes N samples -> STREAMINFO.total_samples == N exactly
 *      (regression test for the /1000 "duration scaling" bug).
 *   2. flac_writer_finalize_streaminfo() is a no-op for counts that fit 36 bits.
 *   3. For counts > 2^36-1 it rewrites total_samples to 0 ("unknown" per spec),
 *      instead of leaving libFLAC's modulo-2^36 wrapped value.
 *
 * Compiled and run by test/ci_guard_tests.py (FLAC STREAMINFO runtime guard).
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flac_writer.h"

#define TEST_SAMPLE_COUNT 123457ULL /* deliberately non-round */

static int read_streaminfo_total_samples(const char *path, uint64_t *out_total) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s for STREAMINFO parse\n", path);
        return -1;
    }
    /* "fLaC" magic (4) + metadata block header (4) + STREAMINFO payload (34) */
    uint8_t buf[42];
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got != sizeof(buf)) {
        fprintf(stderr, "FAIL: %s too short for a FLAC header (%zu bytes)\n", path, got);
        return -1;
    }
    if (memcmp(buf, "fLaC", 4) != 0) {
        fprintf(stderr, "FAIL: %s has no fLaC magic\n", path);
        return -1;
    }
    if ((buf[4] & 0x7F) != 0) { /* block type 0 = STREAMINFO, must be first per spec */
        fprintf(stderr, "FAIL: first metadata block of %s is not STREAMINFO\n", path);
        return -1;
    }
    /* STREAMINFO payload bytes 10..17: sample_rate(20) channels(3) bps(5) total_samples(36) */
    const uint8_t *p = buf + 8 + 10;
    uint64_t packed = 0;
    for (int i = 0; i < 8; i++) packed = (packed << 8) | p[i];
    *out_total = packed & ((((uint64_t)1) << 36) - 1);
    return 0;
}

static int expect_total(const char *path, uint64_t expected, const char *label) {
    uint64_t total = 0;
    if (read_streaminfo_total_samples(path, &total) != 0) return -1;
    if (total != expected) {
        fprintf(stderr, "FAIL: %s: STREAMINFO total_samples = %" PRIu64 ", expected %" PRIu64 "\n",
                label, total, expected);
        return -1;
    }
    printf("PASS: %s (total_samples = %" PRIu64 ")\n", label, total);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "flac_writer_streaminfo_test.flac";

    if (!flac_writer_available()) {
        fprintf(stderr, "FAIL: flac_writer built without libFLAC\n");
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot create %s\n", path);
        return 1;
    }

    flac_writer_config_t config = flac_writer_default_config();
    config.compression_level = 0;
    config.num_threads = 1;

    flac_writer_t *writer = flac_writer_create_file(f, &config);
    if (!writer) {
        fprintf(stderr, "FAIL: flac_writer_create_file failed\n");
        fclose(f);
        remove(path);
        return 1;
    }

    int32_t block[4096];
    uint64_t remaining = TEST_SAMPLE_COUNT;
    uint64_t n = 0;
    while (remaining > 0) {
        uint32_t chunk = remaining < 4096 ? (uint32_t)remaining : 4096;
        for (uint32_t i = 0; i < chunk; i++) {
            block[i] = (int32_t)(int16_t)((n + i) * 2654435761u); /* deterministic noise */
        }
        if (flac_writer_process(writer, block, chunk) < 0) {
            fprintf(stderr, "FAIL: flac_writer_process: %s\n", flac_writer_get_error_string(writer));
            flac_writer_abort(writer);
            remove(path);
            return 1;
        }
        n += chunk;
        remaining -= chunk;
    }

    if (flac_writer_get_samples_written(writer) != TEST_SAMPLE_COUNT) {
        fprintf(stderr, "FAIL: samples_written mismatch\n");
        flac_writer_abort(writer);
        remove(path);
        return 1;
    }

    /* finish() closes the FILE* (init_FILE ownership) and rewrites STREAMINFO */
    if (flac_writer_finish(writer) != FLAC_WRITER_OK) {
        fprintf(stderr, "FAIL: flac_writer_finish failed\n");
        remove(path);
        return 1;
    }

    int rc = 1;
    do {
        if (expect_total(path, TEST_SAMPLE_COUNT, "encoded count is exact (no /1000 scaling)") != 0) break;

        if (!flac_writer_finalize_streaminfo(path, TEST_SAMPLE_COUNT)) {
            fprintf(stderr, "FAIL: finalize_streaminfo returned false for in-range count\n");
            break;
        }
        if (expect_total(path, TEST_SAMPLE_COUNT, "finalize is a no-op below 2^36") != 0) break;

        if (!flac_writer_finalize_streaminfo(path, (((uint64_t)1) << 36) + 123ULL)) {
            fprintf(stderr, "FAIL: finalize_streaminfo returned false for overflow count\n");
            break;
        }
        if (expect_total(path, 0, "overflow count rewrites total_samples to 0 (unknown)") != 0) break;

        rc = 0;
    } while (0);

    remove(path);
    return rc;
}
