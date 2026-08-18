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

#include <FLAC/metadata.h>

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

/* Walks the metadata block headers; returns 0 and fills *out_length if a block
 * of the given type exists, -1 otherwise. */
static int find_metadata_block(const char *path, uint8_t type, uint32_t *out_length) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s for block walk\n", path);
        return -1;
    }
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) {
        fprintf(stderr, "FAIL: %s has no fLaC magic\n", path);
        fclose(f);
        return -1;
    }
    int found = -1;
    for (;;) {
        uint8_t hdr[4];
        if (fread(hdr, 1, 4, f) != 4) break;
        uint8_t block_type = hdr[0] & 0x7F;
        uint32_t length = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
        if (block_type == type) {
            if (out_length) *out_length = length;
            found = 0;
            break;
        }
        if (fseek(f, (long)length, SEEK_CUR) != 0) break;
        if (hdr[0] & 0x80) break; /* last-metadata-block flag */
    }
    fclose(f);
    return found;
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

        uint32_t pad_len = 0;
        if (find_metadata_block(path, 1 /* PADDING */, &pad_len) != 0) {
            fprintf(stderr, "FAIL: encoded file has no PADDING block\n");
            break;
        }
        if (pad_len != 4096) {
            fprintf(stderr, "FAIL: PADDING length %u, expected 4096\n", pad_len);
            break;
        }
        printf("PASS: encoder reserves a 4096-byte PADDING block\n");

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

        /* In-place tag embed: padded file must not need a tempfile rewrite. */
        flac_writer_tag_t tags[2] = {
            { "RF_TOTAL_SAMPLES", "123457" },
            { "DURATION_SECONDS", "3.086425" },
        };
        bool rewrote = true;
        if (!flac_writer_embed_tags(path, tags, 2, &rewrote)) {
            fprintf(stderr, "FAIL: embed_tags failed on padded file\n");
            break;
        }
        if (rewrote) {
            fprintf(stderr, "FAIL: embed_tags rewrote a padded file (should be in place)\n");
            break;
        }
        printf("PASS: embed_tags is in-place on a padded file\n");

        /* Round-trip: read the tags back via the chain API. */
        {
            FLAC__Metadata_Chain *rchain = FLAC__metadata_chain_new();
            FLAC__Metadata_Iterator *riter = FLAC__metadata_iterator_new();
            int tag_ok = 0;
            if (rchain && riter && FLAC__metadata_chain_read(rchain, path)) {
                FLAC__metadata_iterator_init(riter, rchain);
                do {
                    FLAC__StreamMetadata *b = FLAC__metadata_iterator_get_block(riter);
                    if (b && b->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
                        int f1 = FLAC__metadata_object_vorbiscomment_find_entry_from(b, 0, "RF_TOTAL_SAMPLES");
                        int f2 = FLAC__metadata_object_vorbiscomment_find_entry_from(b, 0, "DURATION_SECONDS");
                        tag_ok = (f1 >= 0 && f2 >= 0);
                        break;
                    }
                } while (FLAC__metadata_iterator_next(riter));
            }
            if (riter) FLAC__metadata_iterator_delete(riter);
            if (rchain) FLAC__metadata_chain_delete(rchain);
            if (!tag_ok) {
                fprintf(stderr, "FAIL: embedded tags did not round-trip\n");
                break;
            }
            printf("PASS: embedded tags round-trip\n");
        }

        rc = 0;
    } while (0);

    if (rc == 0) {
        rc = 1;
        char legacy_path[1024];
        snprintf(legacy_path, sizeof(legacy_path), "%s.legacy", path);
        FILE *lf = fopen(legacy_path, "wb");
        flac_writer_config_t lcfg = flac_writer_default_config();
        lcfg.compression_level = 0;
        lcfg.num_threads = 1;
        lcfg.padding_bytes = 0; /* legacy layout: no padding */
        flac_writer_t *lw = lf ? flac_writer_create_file(lf, &lcfg) : NULL;
        if (lw) {
            int32_t z[4096] = {0};
            if (flac_writer_process(lw, z, 4096) == 4096 &&
                flac_writer_finish(lw) == FLAC_WRITER_OK) {
                flac_writer_tag_t ltags[1] = { { "RF_SAMPLE_RATE", "40000000" } };
                bool lrewrote = false;
                if (find_metadata_block(legacy_path, 1, NULL) == 0) {
                    fprintf(stderr, "FAIL: padding_bytes=0 still produced a PADDING block\n");
                } else if (!flac_writer_embed_tags(legacy_path, ltags, 1, &lrewrote)) {
                    fprintf(stderr, "FAIL: embed_tags failed on legacy file\n");
                } else if (!lrewrote) {
                    fprintf(stderr, "FAIL: embed_tags claims in-place on an unpadded file\n");
                } else {
                    printf("PASS: legacy (unpadded) file falls back to rewrite and reports it\n");
                    rc = 0;
                }
            } else {
                fprintf(stderr, "FAIL: legacy encode failed\n");
            }
        } else {
            fprintf(stderr, "FAIL: cannot create legacy test file\n");
            if (lf) fclose(lf);
        }
        remove(legacy_path);
    }

    remove(path);
    return rc;
}
