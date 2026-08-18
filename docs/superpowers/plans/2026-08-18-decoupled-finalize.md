# Decoupled Capture Finalization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finalization of a stopped recording runs fully detached from module globals so a new capture can start immediately, tag embedding is in-place (no whole-file rewrite), and app exit can never abandon an in-flight finalize.

**Architecture:** Three layers. (1) `common/flac_writer.c` reserves a PADDING block at encode time and gains a chain-API `flac_writer_embed_tags()` that grows the VORBIS_COMMENT block into that padding in place. (2) `misrc_gui/output/gui_record.c` moves all per-recording state into a heap-allocated `gui_record_session_t`; the module keeps two pointers (`s_active`, `s_finalizing`), at most one finalize in flight. (3) `misrc_gui/core/misrc_gui.c` holds the window open while a finalize runs and calls the (currently never-called) `gui_record_cleanup()` on teardown.

**Tech Stack:** C11, libFLAC 1.5 (metadata chain API), C11 threads (`thrd_*`), raylib/Clay GUI, Meson build, Python CI guard harness.

**Spec:** `docs/superpowers/specs/2026-08-18-decoupled-finalize-design.md`

## Global Constraints

- Vendored source root is `misrc_tools/`; all paths below are relative to the repo root `/home/rdodge/Repos/MISRC-GUI`.
- Rebuild with `meson compile -C build-fedora misrc_gui misrc_capture misrc_extract`. **Never** rerun `scripts/build-fedora.sh` on the existing build dir (it `--wipe`s the configuration).
- Every commit must leave the build green: run the meson compile after each task's code steps.
- The CI runtime harness is compiled ad hoc by `misrc_tools/test/ci_guard_tests.py` from `misrc_tools/test/flac_writer_streaminfo_test.c` + `misrc_tools/common/flac_writer.c` — no meson test targets exist and none are added.
- Run the harness locally exactly the way CI does:
  `python3 misrc_tools/test/ci_guard_tests.py` (full preflight; the "FLAC STREAMINFO total_samples runtime" check compiles and runs the harness). `--static-only` skips it.
- Branch `fix/decoupled-finalize` (already created, spec committed). Push target is the GDH fork **identified by URL** via `git remote -v` (`git@github.com:GDH-Technologies/MISRC-GUI.git`); never push to harrypm upstream.
- Commit style: conventional-ish, matching `fix(flac): stop scaling STREAMINFO total_samples; handle 2^36 overflow`.
- `LIBFLAC_ENABLED == 1` guards all libFLAC code; every new public `flac_writer_*` symbol needs a stub in the `#else` section of `common/flac_writer.c` (currently lines 733-791).
- New RF FLAC file layout after this plan: `STREAMINFO, VORBIS_COMMENT(vendor), SEEKTABLE, PADDING(4096)`.
- The five duration tags and their exact formats (must not change): `DURATION_SECONDS` (`%.6f`), `LENGTH` (ms, `llround(seconds*1000)`), `RF_TOTAL_SAMPLES`, `RF_SAMPLE_RATE` (Hz), `RF_SAMPLE_RATE_KHZ`.

---

### Task 1: PADDING block at encode time (`flac_writer.c`)

**Files:**
- Modify: `misrc_tools/common/flac_writer.h` (config struct ~line 57-79, doc comment of `flac_writer_default_config`)
- Modify: `misrc_tools/common/flac_writer.c` (writer struct ~line 237-257, `flac_writer_default_config` ~line 362-378, `configure_encoder` ~line 383-437, cleanup paths in `flac_writer_create_file`/`flac_writer_create_stream`/`flac_writer_finish`/`flac_writer_abort`)
- Test: `misrc_tools/test/flac_writer_streaminfo_test.c`

**Interfaces:**
- Consumes: existing `flac_writer_config_t`, `configure_encoder()`.
- Produces: `flac_writer_config_t.padding_bytes` (`uint32_t`, default 4096, 0 = no padding). Encoded files gain a trailing `PADDING` metadata block. Task 2's in-place embed and Task 3's GUI rely on this.

- [ ] **Step 1: Write the failing test**

In `misrc_tools/test/flac_writer_streaminfo_test.c`, add a metadata-block walker below `read_streaminfo_total_samples()`:

```c
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
```

In `main()`, inside the `do { ... } while (0)` chain, after the existing `expect_total(path, TEST_SAMPLE_COUNT, "encoded count is exact ...")` check, add:

```c
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
```

- [ ] **Step 2: Run the harness to verify it fails**

```bash
cd /home/rdodge/Repos/MISRC-GUI && python3 misrc_tools/test/ci_guard_tests.py 2>&1 | tail -20
```
Expected: `FAILED: FLAC STREAMINFO total_samples runtime` with `FAIL: encoded file has no PADDING block` in the harness output.

- [ ] **Step 3: Implement padding in flac_writer**

`misrc_tools/common/flac_writer.h` — in `flac_writer_config_t`, after the seektable fields:

```c
    // Trailing PADDING block reserved at encode time so post-capture metadata
    // (vorbis tags) can be embedded in place instead of rewriting the whole
    // file. 0 disables. Default: 4096 bytes.
    uint32_t padding_bytes;
```

Update the `flac_writer_default_config()` doc comment to mention "4096B padding".

`misrc_tools/common/flac_writer.c`:

1. In `struct flac_writer` (after `FLAC__StreamMetadata *seektable;`):
```c
    FLAC__StreamMetadata *padding;
```
2. In `flac_writer_default_config()` initializer (after `.seektable_spacing = ...`):
```c
        .padding_bytes = 4096,
```
3. In `configure_encoder()`, replace the seektable block (lines 414-434, from `// Seektable` through its closing brace) with a combined array build — the existing seektable code kept intact except `FLAC__stream_encoder_set_metadata` moves out of it:

```c
    // Metadata blocks: seektable, then trailing padding (absorbed later by
    // in-place tag embedding -- see flac_writer_embed_tags).
    FLAC__StreamMetadata *meta[2];
    uint32_t n_meta = 0;

    if (writer->config.enable_seektable) {
        writer->seektable = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
        if (!writer->seektable) {
            report_error(writer, FLAC_WRITER_ERR_SEEKTABLE, "Failed to allocate seektable");
            return FLAC_WRITER_ERR_SEEKTABLE;
        }

        uint32_t spacing = writer->config.seektable_spacing;
        if (spacing == 0) spacing = 1 << 18;

        // Estimate for very long recordings (up to ~1.5 years at 40kHz)
        if (!FLAC__metadata_object_seektable_template_append_spaced_points(
                writer->seektable, spacing, (uint64_t)1 << 41)) {
            report_error(writer, FLAC_WRITER_ERR_SEEKTABLE, "Failed to configure seektable");
            FLAC__metadata_object_delete(writer->seektable);
            writer->seektable = NULL;
            return FLAC_WRITER_ERR_SEEKTABLE;
        }
        meta[n_meta++] = writer->seektable;
    }

    if (writer->config.padding_bytes > 0) {
        writer->padding = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
        if (!writer->padding) {
            report_error(writer, FLAC_WRITER_ERR_CONFIG, "Failed to allocate padding block");
            return FLAC_WRITER_ERR_CONFIG;
        }
        writer->padding->length = writer->config.padding_bytes;
        meta[n_meta++] = writer->padding;
    }

    if (n_meta > 0 && !FLAC__stream_encoder_set_metadata(enc, meta, n_meta)) {
        report_error(writer, FLAC_WRITER_ERR_CONFIG, "Failed to set encoder metadata");
        return FLAC_WRITER_ERR_CONFIG;
    }
```
4. Every cleanup site that deletes `writer->seektable` gets a padding twin. There are five: two in `flac_writer_create_file` (config-error and init-error paths), two in `flac_writer_create_stream` (same), one each in `flac_writer_finish` and `flac_writer_abort`. Pattern at each:
```c
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        if (writer->padding) FLAC__metadata_object_delete(writer->padding);
```
(The disabled-`#else` stubs need no change: `flac_writer_default_config` there returns `{0}` → `padding_bytes = 0`.)

- [ ] **Step 4: Run the harness to verify it passes**

```bash
cd /home/rdodge/Repos/MISRC-GUI && python3 misrc_tools/test/ci_guard_tests.py 2>&1 | tail -8
```
Expected: `PASS: FLAC STREAMINFO total_samples runtime` (harness prints `PASS: encoder reserves a 4096-byte PADDING block`), all other checks PASS.

- [ ] **Step 5: Build all three tools**

```bash
meson compile -C build-fedora misrc_gui misrc_capture misrc_extract
```
Expected: success, no new warnings in `flac_writer.c`.

- [ ] **Step 6: Commit**

```bash
git add misrc_tools/common/flac_writer.h misrc_tools/common/flac_writer.c misrc_tools/test/flac_writer_streaminfo_test.c
git commit -m "feat(flac): reserve trailing PADDING block at encode time"
```

---

### Task 2: `flac_writer_embed_tags()` — in-place vorbis tag embedding

**Files:**
- Modify: `misrc_tools/common/flac_writer.h` (new type + prototype, next to `flac_writer_finalize_streaminfo`)
- Modify: `misrc_tools/common/flac_writer.c` (implementation after `flac_writer_finalize_streaminfo` ~line 688; stub in `#else` section)
- Test: `misrc_tools/test/flac_writer_streaminfo_test.c`

**Interfaces:**
- Consumes: Task 1's padded file layout; libFLAC chain API (same pattern as `flac_writer_finalize_streaminfo`, `flac_writer.c:661-688`).
- Produces:
```c
typedef struct { const char *key; const char *value; } flac_writer_tag_t;
bool flac_writer_embed_tags(const char *path, const flac_writer_tag_t *tags,
                            size_t n_tags, bool *rewrote);
```
Task 3's GUI wrapper calls this. `*rewrote` is set true when the write needed a whole-file rewrite (legacy file without padding), false when in place.

- [ ] **Step 1: Write the failing tests**

In the harness `main()`'s `do { ... } while (0)` chain, after the final `expect_total(path, 0, "overflow count rewrites ...")` check, add (the harness already links libFLAC, so it may use `FLAC__metadata_*` directly for verification — add `#include <FLAC/metadata.h>` at the top of the file):

```c
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
```

Then, after the `do/while` block but before `remove(path); return rc;`, add a second scenario (only runs when the first suite passed) exercising the legacy no-padding fallback with a separate file:

```c
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
                if (lf) { /* finish() already freed lw on success path only */ }
            }
        } else {
            fprintf(stderr, "FAIL: cannot create legacy test file\n");
            if (lf) fclose(lf);
        }
        remove(legacy_path);
    }
```

- [ ] **Step 2: Run the harness to verify it fails**

```bash
cd /home/rdodge/Repos/MISRC-GUI && python3 misrc_tools/test/ci_guard_tests.py 2>&1 | tail -20
```
Expected: harness compile FAILS with `implicit declaration of function 'flac_writer_embed_tags'` / unknown type `flac_writer_tag_t` (a compile failure of the guard is the failing state here).

- [ ] **Step 3: Implement `flac_writer_embed_tags`**

`misrc_tools/common/flac_writer.h`, directly after the `flac_writer_finalize_streaminfo` prototype:

```c
// One vorbis comment for flac_writer_embed_tags().
typedef struct {
    const char *key;
    const char *value;
} flac_writer_tag_t;

// Appends key=value comments to the file's VORBIS_COMMENT block (creating one
// after STREAMINFO if absent). With the trailing PADDING block reserved at
// encode time this is an in-place metadata write (milliseconds); on legacy
// files without padding libFLAC falls back to rewriting the whole file through
// a ".metadata_edit" temp. *rewrote (optional) reports which path was taken so
// callers can warn about slow legacy rewrites. File mtime is preserved.
// Returns true on success.
bool flac_writer_embed_tags(const char *path, const flac_writer_tag_t *tags,
                            size_t n_tags, bool *rewrote);
```

`misrc_tools/common/flac_writer.c`, after `flac_writer_finalize_streaminfo` (line 688):

```c
/* ============================================================================
 * Post-close vorbis tag embedding (in-place via reserved padding)
 * ============================================================================ */
bool flac_writer_embed_tags(const char *path, const flac_writer_tag_t *tags,
                            size_t n_tags, bool *rewrote) {
    if (rewrote) *rewrote = false;
    if (!path || !path[0] || !tags || n_tags == 0) return false;

    FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
    if (!chain) return false;

    bool success = false;
    if (FLAC__metadata_chain_read(chain, path)) {
        FLAC__Metadata_Iterator *iter = FLAC__metadata_iterator_new();
        if (iter) {
            FLAC__metadata_iterator_init(iter, chain);
            FLAC__StreamMetadata *vc = NULL;
            do {
                FLAC__StreamMetadata *b = FLAC__metadata_iterator_get_block(iter);
                if (b && b->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
                    vc = b;
                    break;
                }
            } while (FLAC__metadata_iterator_next(iter));

            bool created_vc = false;
            if (!vc) {
                vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
                created_vc = (vc != NULL);
            }

            bool ok = (vc != NULL);
            for (size_t i = 0; ok && i < n_tags; i++) {
                FLAC__StreamMetadata_VorbisComment_Entry entry;
                if (!FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
                        &entry, tags[i].key, tags[i].value)) {
                    ok = false;
                    break;
                }
                /* copy=false transfers entry ownership to the block on success */
                if (!FLAC__metadata_object_vorbiscomment_append_comment(vc, entry, false)) {
                    free(entry.entry);
                    ok = false;
                }
            }

            if (ok && created_vc) {
                /* Insert the new block directly after STREAMINFO. */
                FLAC__metadata_iterator_init(iter, chain);
                ok = FLAC__metadata_iterator_insert_block_after(iter, vc);
            }
            if (!ok && created_vc && vc) {
                FLAC__metadata_object_delete(vc);
            }

            if (ok) {
                if (rewrote) {
                    *rewrote = FLAC__metadata_chain_check_if_tempfile_needed(
                        chain, /*use_padding=*/true);
                }
                success = FLAC__metadata_chain_write(chain, /*use_padding=*/true,
                                                     /*preserve_file_stats=*/true);
            }
            FLAC__metadata_iterator_delete(iter);
        }
    }
    FLAC__metadata_chain_delete(chain);
    return success;
}
```

Stub in the `#else // LIBFLAC_ENABLED != 1` section, next to the `flac_writer_finalize_streaminfo` stub:

```c
bool flac_writer_embed_tags(const char *path, const flac_writer_tag_t *tags,
                            size_t n_tags, bool *rewrote) {
    (void)path; (void)tags; (void)n_tags;
    if (rewrote) *rewrote = false;
    return false;
}
```

- [ ] **Step 4: Run the harness to verify it passes**

```bash
cd /home/rdodge/Repos/MISRC-GUI && python3 misrc_tools/test/ci_guard_tests.py 2>&1 | tail -10
```
Expected: all PASS lines including `PASS: embed_tags is in-place on a padded file`, `PASS: embedded tags round-trip`, `PASS: legacy (unpadded) file falls back to rewrite and reports it`.

- [ ] **Step 5: Build and commit**

```bash
meson compile -C build-fedora misrc_gui misrc_capture misrc_extract
git add misrc_tools/common/flac_writer.h misrc_tools/common/flac_writer.c misrc_tools/test/flac_writer_streaminfo_test.c
git commit -m "feat(flac): add flac_writer_embed_tags with in-place padded write"
```

---

### Task 3: GUI duration tags go through `flac_writer_embed_tags`

**Files:**
- Modify: `misrc_tools/misrc_gui/output/gui_record.c` — replace the body of `gui_record_embed_flac_duration_metadata()` (lines 1729-1814)

**Interfaces:**
- Consumes: `flac_writer_embed_tags()` from Task 2.
- Produces: same function signature as today (`static void gui_record_embed_flac_duration_metadata(gui_app_t *app, const char *path, const char *channel_label, uint64_t total_samples, uint32_t sample_rate_hz)`); call sites in the finalize path (`gui_record.c:3206`, `:3212`) are untouched.

- [ ] **Step 1: Rewrite the function**

Replace everything from the `FLAC__Metadata_SimpleIterator *it = ...` line (1752) through the end of the function (1814) — keeping the value-formatting block (1735-1750) exactly as is — with:

```c
    flac_writer_tag_t tags[5] = {
        { "DURATION_SECONDS",   duration_seconds_str },
        { "LENGTH",             length_ms_str },
        { "RF_TOTAL_SAMPLES",   total_samples_str },
        { "RF_SAMPLE_RATE",     sample_rate_str },
        { "RF_SAMPLE_RATE_KHZ", sample_rate_khz_str },
    };
    bool rewrote = false;
    if (!flac_writer_embed_tags(path, tags, 5, &rewrote)) {
        if (app) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Failed writing FLAC duration metadata for %s (%s)",
                     channel_label ? channel_label : "RF", path);
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
        return;
    }
    if (rewrote && app) {
        /* Legacy file without reserved padding: libFLAC rewrote the whole
         * file, which takes minutes on full-tape captures. */
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "FLAC duration metadata for %s required a full-file rewrite "
                 "(no padding block; capture predates padded layout): %s",
                 channel_label ? channel_label : "RF", path);
        gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
    }
```

The `FLAC__metadata_simple_iterator_*` code and its three error branches are deleted entirely.

- [ ] **Step 2: Build**

```bash
meson compile -C build-fedora misrc_gui
```
Expected: success. Then confirm the simple-iterator API is gone from the file:
```bash
grep -c "metadata_simple_iterator" misrc_tools/misrc_gui/output/gui_record.c
```
Expected: `0`.

- [ ] **Step 3: Commit**

```bash
git add misrc_tools/misrc_gui/output/gui_record.c
git commit -m "refactor(gui): embed duration tags via flac_writer_embed_tags (in-place)"
```

---

### Task 4: Introduce `gui_record_session_t` (files, writers, paths, log, video latch, timing)

This task migrates state into the session struct **without changing behavior**: the finalize gates stay in place until Task 6, so at every commit the app behaves exactly as today. The session is heap-allocated at record start and freed after finalize is reaped.

**Files:**
- Modify: `misrc_tools/misrc_gui/output/gui_record.c` throughout (globals block ~380-490, log helpers ~405-461, finalize ctx ~470-484, `gui_record_start_confirmed` ~2624-3119, `gui_record_finalize_stop_sync` ~3122-3297, `gui_record_finalize_thread` ~3299-3312, `gui_record_stop` ~3314-3363, `gui_record_cleanup` ~1546-1557, `gui_record_is_active` ~1560-1562)

**Interfaces:**
- Consumes: existing `writer_ctx_t` (line 937-968), spill globals (migrated in Task 5, still global in this task).
- Produces (module-internal, used by Tasks 5-6):

```c
typedef struct gui_record_session {
    gui_app_t *app;

    /* Latched at start -- finalize must never read live settings. */
    bool use_flac;
    bool capture_a, capture_b;
    bool video_started;

    FILE *file_a, *file_b;
    char path_a[512], path_b[512];
#if LIBFLAC_ENABLED == 1
    flac_writer_t *flac_a, *flac_b;
    uint32_t sample_rate_a, sample_rate_b;
#endif

    thrd_t writer_thread_a, writer_thread_b;
    bool writer_threads_running;
    writer_ctx_t ctx_a, ctx_b;

    FILE *log_file;
    char log_path[512];

    /* Timing and stat snapshots -- end_* and raw/comp filled at stop time
     * (Task 6); until then finalize still reads live atomics. */
    double recording_start_time;
    double stop_request_time;
    uint32_t start_rec_a_waits, start_rec_a_drops;
    uint32_t start_rec_b_waits, start_rec_b_drops;
} gui_record_session_t;

static gui_record_session_t *s_active = NULL;      /* recording in progress */
static gui_record_session_t *s_finalizing = NULL;  /* handed to finalize thread */
```

- Log helpers become session-explicit at the core, ambient at the edge:
  - `static void gui_record_log_write_line_ses(gui_record_session_t *ses, const char *level, const char *message)` — writes to `ses->log_file` under the existing global `s_capture_log_lock`.
  - `static void gui_record_log_writef_ses(gui_record_session_t *ses, const char *level, const char *format, ...)` — varargs wrapper.
  - `static void gui_record_close_session_log_ses(gui_record_session_t *ses)` — closes `ses->log_file`.
  - Existing `gui_record_log_write_line` / `gui_record_log_writef` keep their signatures and forward to the `_ses` variants with `s_active` (ambient events during recording land in the active session's log, as today).
- `gui_record_finalize_stop_sync(gui_record_session_t *ses)` — replaces the `(gui_app_t *app, double stop_request_time)` signature; `app` and `stop_request_time` come from the session.
- `gui_record_finalize_ctx_t` is deleted; the finalize thread receives the `gui_record_session_t *` directly.

**Migration table** (every global on the left is deleted; `grep -n` for each and route through the session):

| delete global (line) | becomes |
|---|---|
| `s_writer_thread_a/b` (384-385) | `ses->writer_thread_a/b` |
| `s_writer_threads_running` (388) | `ses->writer_threads_running` |
| `s_video_started` (392) | `ses->video_started` |
| `s_file_a/b` (397-398) | `ses->file_a/b` |
| `s_record_path_a/b` (399-400) | `ses->path_a/b` |
| `s_record_sample_rate_a/b` (402-403) | `ses->sample_rate_a/b` |
| `s_capture_log_file` / `s_capture_log_path` (405-406) | `ses->log_file` / `ses->log_path` |
| `s_recording_app` (464) | `s_active->app` (and `s_active` non-NULL replaces the pointer test) |
| `s_start_rec_*` (487-490) | `ses->start_rec_*` |
| `s_ctx_a/b` (1025-1026) | `ses->ctx_a/b` |
| `s_flac_writer_a/b` (1030-1031) | `ses->flac_a/b` |

`s_record_path_video` (393) and `s_video_start_msg` (394) stay global for now — they are written by the video preflight before the session exists; move `s_record_path_video` into the session in Task 6 (collision guard needs it), latched right after session allocation.

- [ ] **Step 1: Add the struct + pointers, delete the globals, convert the log helpers**

Insert the struct definition after the `writer_ctx_t` definition (line 968) so `writer_ctx_t` is complete, with the two static pointers. Delete the globals per the table. Convert log helpers:

```c
static void gui_record_close_session_log_ses(gui_record_session_t *ses) {
    if (!ses) return;
    gui_record_log_lock();
    if (ses->log_file) {
        fclose(ses->log_file);
        ses->log_file = NULL;
    }
    ses->log_path[0] = '\0';
    gui_record_log_unlock();
}

static void gui_record_log_write_line_ses(gui_record_session_t *ses,
                                          const char *level, const char *message) {
    if (!ses || !message || !message[0]) return;
    gui_record_log_lock();
    if (ses->log_file) {
        char ts[32];
        gui_record_build_log_timestamp(ts, sizeof(ts));
        fprintf(ses->log_file, "[%s] [%s] %s\n", ts,
                (level && level[0]) ? level : "INFO", message);
        fflush(ses->log_file);
    }
    gui_record_log_unlock();
}

static void gui_record_log_writef_ses(gui_record_session_t *ses,
                                      const char *level, const char *format, ...) {
    if (!ses || !format || !format[0]) return;
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    gui_record_trim_trailing_newline(message);
    if (!message[0]) return;
    gui_record_log_write_line_ses(ses, level, message);
}
```

Ambient forwarders (replace the old bodies):

```c
static void gui_record_close_session_log(void) {
    gui_record_close_session_log_ses(s_active);
}

static void gui_record_log_write_line(const char *level, const char *message) {
    gui_record_log_write_line_ses(s_active, level, message);
}
```

`gui_record_log_writef` keeps its body but its final line calls `gui_record_log_write_line(level, message)` (unchanged). `gui_record_open_session_log(...)` (find with `grep -n "gui_record_open_session_log" `) changes its stores of `s_capture_log_file`/`s_capture_log_path` to `s_active->log_file`/`s_active->log_path`.

NOTE: the struct definition must appear *before* the log helpers (currently at 405-461) reference it — forward-declare above them:
```c
typedef struct gui_record_session gui_record_session_t;
static gui_record_session_t *s_active;
static gui_record_session_t *s_finalizing;
```
near line 396, with the full struct + the actual definitions `static gui_record_session_t *s_active = NULL; static gui_record_session_t *s_finalizing = NULL;` after `writer_ctx_t`. (C allows tentative definitions; keep the initialized definitions in exactly one place — after the struct — and make the early ones `extern`-free declarations by writing them once. Simplest correct form: put only the `typedef struct gui_record_session gui_record_session_t;` forward typedef early, move the log helper *definitions* below the struct, and leave early callers with forward *declarations* of the helpers.)

- [ ] **Step 2: Migrate `gui_record_start_confirmed`**

At the top of the FLAC/RAW common section (right after the video preflight, replacing `s_recording_app = app;` at line 2753):

```c
    gui_record_session_t *ses = calloc(1, sizeof(*ses));
    if (!ses) {
        gui_app_set_status(app, "Out of memory starting recording");
        return RECORD_ERROR;
    }
    ses->app = app;
    ses->use_flac = false;
    ses->capture_a = app->settings.capture_a;
    ses->capture_b = app->settings.capture_b;
    ses->video_started = false;
    s_active = ses;
```

Then mechanically: within `gui_record_start_confirmed`, every occurrence per the migration table becomes `ses->...`; the `s_record_path_a/b` latching at 2632-2643 moves onto `ses->path_a/b`; the FLAC branch sets `ses->use_flac = true;` and `ses->sample_rate_a/b`; `s_ctx_a.` → `ses->ctx_a.` etc.; thread creates use `&ses->writer_thread_a`, `&ses->ctx_a`; `s_writer_threads_running = started_a || started_b;` → `ses->writer_threads_running = ...`. Every error-return path that today does `s_recording_app = NULL;` instead does:

```c
        gui_record_close_session_log_ses(ses);
        free(ses);
        s_active = NULL;
        return RECORD_ERROR;
```

(ordered after the writer aborts/fcloses that path already performs). `s_video_started` in `gui_record_start_video_if_enabled` and its uses (2697, 2939, 3131) become `s_active->video_started` where they run at start, `ses->video_started` inside finalize.

- [ ] **Step 3: Migrate the finalize path**

`gui_record_finalize_stop_sync` becomes:

```c
static void gui_record_finalize_stop_sync(gui_record_session_t *ses) {
    gui_app_t *app = ses->app;
    double stop_request_time = ses->stop_request_time;
```

and inside, per the table: writer joins use `ses->writer_thread_a/b` gated on `ses->capture_a/b` (latched — this also fixes the live-settings hazard the comment at 3128-3130 warns about); `s_video_started` → `ses->video_started`; FLAC finish block uses `ses->flac_a/b`; fcloses use `ses->file_a/b`; the metadata section (3202-3215) reads `ses->use_flac`, `ses->capture_a/b`, `ses->path_a/b`, `ses->sample_rate_a/b`; the summary block's `gui_record_log_writef(...)` calls all become `gui_record_log_writef_ses(ses, ...)` and the closing `gui_record_close_session_log()` becomes `gui_record_close_session_log_ses(ses)`; the trailing `s_record_path_a[0]='\0'` / sample-rate resets / `s_recording_app = NULL` lines are deleted (the session is freed by the reaper).

For this task (gates still in place) the stats block keeps reading the live atomics — change only the baseline reads from `s_start_rec_*` to `ses->start_rec_*`. `app->recording_start_time` reads become `ses->recording_start_time` (set in start where `app->recording_start_time = GetTime()` happens — set both: the app field stays because the UI elapsed-time display reads it).

`gui_record_finalize_thread` becomes:

```c
static int gui_record_finalize_thread(void *arg) {
    gui_record_session_t *ses = (gui_record_session_t *)arg;
    gui_record_finalize_stop_sync(ses);
    atomic_store(&s_record_stop_finalize_done, true);
    atomic_store(&s_record_stop_finalizing, false);
    return 0;
}
```

`gui_record_stop` (3314-3363): delete the `gui_record_finalize_ctx_t` allocation; instead:

```c
    gui_record_session_t *ses = s_active;
    if (!ses) { app->is_recording = false; return; }
    ses->stop_request_time = GetTime();
    ...existing stop sequence (extract off, is_recording=false, audio, priority)...
    s_active = NULL;
    s_finalizing = ses;
    atomic_store(&s_record_stop_finalize_done, false);
    atomic_store(&s_record_stop_finalizing, true);
    if (thrd_create(&s_finalize_thread, gui_record_finalize_thread, ses) != thrd_success) {
        atomic_store(&s_record_stop_finalizing, false);
        gui_record_finalize_stop_sync(ses);
        free(ses);
        s_finalizing = NULL;
        gui_app_set_status(app, "Recording stopped");
        return;
    }
    atomic_store(&s_finalize_thread_running, true);
    gui_app_set_status(app, "Finalizing recording...");
```

The reaper frees the session:

```c
static bool gui_record_collect_finalize_if_done(void) {
    if (!atomic_exchange(&s_record_stop_finalize_done, false)) {
        return false;
    }
    if (atomic_load(&s_finalize_thread_running)) {
        thrd_join(s_finalize_thread, NULL);
        atomic_store(&s_finalize_thread_running, false);
    }
    if (s_finalizing) {
        free(s_finalizing);
        s_finalizing = NULL;
    }
    return true;
}
```

`gui_record_cleanup` (1546-1557) gets the same session free after its join (add `if (s_finalizing) { free(s_finalizing); s_finalizing = NULL; }` after the join block). `gui_record_is_active` becomes:

```c
bool gui_record_is_active(void) {
    return s_active != NULL && s_active->app->is_recording;
}
```

- [ ] **Step 4: Build + zero-straggler check**

```bash
meson compile -C build-fedora misrc_gui
for sym in s_recording_app s_file_a s_file_b s_record_path_a s_record_path_b \
           s_record_sample_rate_a s_record_sample_rate_b s_capture_log_file \
           s_writer_thread_a s_writer_thread_b s_writer_threads_running \
           s_video_started s_ctx_a s_ctx_b s_flac_writer_a s_flac_writer_b \
           s_start_rec_a_wait_count; do
  echo "$sym: $(grep -c "\b$sym\b" misrc_tools/misrc_gui/output/gui_record.c)"
done
```
Expected: compile success; every counter prints `0`.

- [ ] **Step 5: Smoke-run**

```bash
./build-fedora/misrc_gui & sleep 5; kill %1
```
Expected: app launches and exits without crash (no device needed for launch).

- [ ] **Step 6: Commit**

```bash
git add misrc_tools/misrc_gui/output/gui_record.c
git commit -m "refactor(gui): move per-recording state into gui_record_session_t"
```

---

### Task 5: Session-owned spill channels, write-error flags, and stat snapshots

**Files:**
- Modify: `misrc_tools/misrc_gui/output/gui_record.c` (spill block ~492-830, writer threads `flac_writer_thread`/`raw_writer_thread` ~1113-1535, `gui_record_has_write_error` ~1565-1570, disk-guard/backlog helpers, finalize summary block ~3218-3243)

**Interfaces:**
- Consumes: `gui_record_session_t` from Task 4.
- Produces: `writer_ctx_t` gains `gui_record_session_t *ses;` (set at start alongside `.app`). Session gains:
```c
    gui_record_spill_channel_t spill[GUI_RECORD_SPILL_CHANNELS];
    atomic_bool write_error[GUI_RECORD_SPILL_CHANNELS];
    uint32_t end_rec_a_waits, end_rec_a_drops, end_rec_b_waits, end_rec_b_drops;
    atomic_uint_fast64_t acc_raw[GUI_RECORD_SPILL_CHANNELS];
    atomic_uint_fast64_t acc_comp[GUI_RECORD_SPILL_CHANNELS];
```
(The spill struct definition moves above `gui_record_session_t` if not already.)

- [ ] **Step 1: Migrate spill + write-error state**

Delete `static gui_record_spill_channel_t s_record_spill[...]` (516) and `static atomic_bool s_record_write_error[...]` (522-523). Every spill helper that today indexes `s_record_spill[channel]` takes a leading `gui_record_session_t *ses` parameter and indexes `ses->spill[channel]`; same for `s_record_write_error` → `ses->write_error`. Route the call sites:

- Writer-thread paths (`flac_writer_thread`, `raw_writer_thread`, `gui_record_get_next_block`, spill write/drain/recycle): pass `wctx->ses`.
- Recording-start reset (`gui_record_spill_reset_all` at 2760 and write-error clears at 2762-2764): operate on the fresh `ses` before threads start — because a freshly calloc'd session's `atomic_flag io_lock` and atomics need explicit init, give the session an initializer called from `gui_record_start_confirmed` right after `calloc`:

```c
static void gui_record_session_init_sync(gui_record_session_t *ses) {
    for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
        atomic_flag_clear(&ses->spill[i].io_lock);
        atomic_store(&ses->spill[i].opened, false);
        atomic_store(&ses->spill[i].forced_mode, false);
        atomic_store(&ses->spill[i].write_offset, 0);
        atomic_store(&ses->spill[i].read_offset, 0);
        atomic_store(&ses->spill[i].last_backlog_log_mark, 0);
        atomic_store(&ses->write_error[i], false);
    }
}
```
(match the field list against the actual `gui_record_spill_channel_t` definition at ~500-514 and initialize every atomic it contains).
- Finalize's `gui_record_spill_reset_all()` (3157) → `gui_record_spill_reset_all(ses)` closing that session's spill files.
- UI/disk-guard queries (`gui_record_spill_backlog_total_bytes`, BUFMGR display, disk guard): these concern the live recording — read `s_active` (return 0 when `s_active == NULL`).
- `gui_record_has_write_error()` must reflect both live and finalizing sessions (the finalize icon blinks red during finalize):

```c
bool gui_record_has_write_error(void) {
    gui_record_session_t *sessions[2] = { s_active, s_finalizing };
    for (int s = 0; s < 2; s++) {
        if (!sessions[s]) continue;
        for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
            if (atomic_load(&sessions[s]->write_error[i])) return true;
        }
    }
    return false;
}
```
- `gui_record_init` (1538-1543) and `gui_record_cleanup` (1554-1555) call `gui_record_spill_reset_all(...)` on globals that no longer exist — in `gui_record_init` drop the call (no sessions exist yet); in `gui_record_cleanup` drop it too (the join + free already ran the finalize which closed its spill files).

- [ ] **Step 2: Snapshot stats at stop time**

Two different snapshot semantics, handled differently:

**Buffer wait/drop counters** are per-buffer-manager and genuinely shared — a new recording resets nothing but continues incrementing them, so the correct end-of-session value is a stop-time snapshot. In `gui_record_stop`, right after `ses->stop_request_time = GetTime();`:

```c
    // Snapshot buffer-stat counters NOW: a new recording keeps incrementing
    // the shared buffer-manager counters while this session finalizes.
    ses->end_rec_a_waits = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
    ses->end_rec_a_drops = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
    ses->end_rec_b_waits = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
    ses->end_rec_b_drops = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);
```

**Raw/compressed byte totals** must keep counting until the writer threads finish draining, so they accumulate in the session, not the app. Delete the `ses->raw_a/raw_b/comp_a/comp_b` idea entirely; instead add to the session:

```c
    atomic_uint_fast64_t acc_raw[GUI_RECORD_SPILL_CHANNELS];
    atomic_uint_fast64_t acc_comp[GUI_RECORD_SPILL_CHANNELS];
```

Repoint the writer-thread accumulation at the session: `writer_ctx_t.compressed_bytes` already carries a pointer (today `&app->recording_compressed_a/b`, set at `gui_record.c:2796`/`:2814`) — set it to `&ses->acc_comp[channel]` instead, and where the compressed count feeds the UI ratio display, mirror-write the app atomic as well. Find the raw-bytes accumulation sites with `grep -n "recording_raw_a\|recording_raw_b" misrc_tools/misrc_gui/output/gui_record.c misrc_tools/misrc_gui/processing/*.c misrc_tools/misrc_gui/input/*.c` and at each writer-side increment write both the app atomic (UI display keeps working) and `ses->acc_raw[channel]` via `wctx->ses`. `gui_record_finalize_stop_sync` then reads exact post-drain totals after the writer joins:

```c
    uint64_t raw_a  = atomic_load(&ses->acc_raw[0]);
    uint64_t raw_b  = atomic_load(&ses->acc_raw[1]);
    uint64_t comp_a = atomic_load(&ses->acc_comp[0]);
    uint64_t comp_b = atomic_load(&ses->acc_comp[1]);
```

In `gui_record_finalize_stop_sync`'s summary block (3227-3243), replace the live atomic loads with the session fields above and the `end_*` locals with `ses->end_rec_*`.

- [ ] **Step 3: Build + straggler check + smoke-run**

```bash
meson compile -C build-fedora misrc_gui
grep -c "s_record_spill\b\|s_record_write_error\b" misrc_tools/misrc_gui/output/gui_record.c   # expect 0
./build-fedora/misrc_gui & sleep 5; kill %1
```

- [ ] **Step 4: Commit**

```bash
git add misrc_tools/misrc_gui/output/gui_record.c
git commit -m "refactor(gui): session-owned spill, write-error and stat snapshots"
```

---

### Task 6: Remove the gates — record while finalizing

**Files:**
- Modify: `misrc_tools/misrc_gui/output/gui_record.c` (`gui_record_start` gate 2519-2524, `gui_record_stop` gate 3318-3321, video path latch)
- Modify: `misrc_tools/misrc_gui/ui/gui_ui.c` (record button ~3468-3478, click handler ~5901-5908)

**Interfaces:**
- Consumes: session pointers from Tasks 4-5.
- Produces: `gui_record_is_finalizing()` unchanged (UI indicator). New refusal statuses: `"Previous recording is still finalizing these files"`, `"Previous recording still finalizing reference video..."`, `"Waiting for previous finalize..."`.

- [ ] **Step 1: Latch the video path into the session**

Move `s_record_path_video` into the session as `char path_video[600];`. The preflight (2697-2698) still clears the global buffers it writes today — after session allocation, copy: `snprintf(ses->path_video, sizeof(ses->path_video), "%s", s_record_path_video);` at the point the video is actually started (`gui_record_start_video_if_enabled`), so `ses->path_video[0] != '\0'` iff this session records video. (Keep the global as the staging buffer the preflight writes; only the latched copy lives in the session.)

- [ ] **Step 2: Replace the start gate with the collision guard**

In `gui_record_start` (2519-2524), replace:

```c
    (void)gui_record_collect_finalize_if_done();

    if (atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running)) {
        gui_app_set_status(app, "Finalizing previous recording...");
        return RECORD_ERROR;
    }
```

with:

```c
    (void)gui_record_collect_finalize_if_done();
```

Then, after the output paths are built (existing `path_a`/`path_b` snprintfs at 2544-2547 — and note the function builds `path_video` at 2555-2557), insert the guard:

```c
    // A finalizing session still owns its output files; refuse to reuse them.
    if (s_finalizing) {
        char new_video[600];
        snprintf(new_video, sizeof(new_video), "%s/%s",
                 app->settings.output_path, app->settings.video_filename);
        bool clash =
            (app->settings.capture_a && s_finalizing->path_a[0] &&
             strcmp(path_a, s_finalizing->path_a) == 0) ||
            (app->settings.capture_b && s_finalizing->path_b[0] &&
             strcmp(path_b, s_finalizing->path_b) == 0);
        if (clash) {
            gui_app_set_status(app, "Previous recording is still finalizing these files");
            return RECORD_ERROR;
        }
        // The reference-video encoder is a single global instance; its finish
        // runs inside finalize, so a new video-enabled recording must wait.
        if (app->settings.video_record_enabled && s_finalizing->video_started) {
            gui_app_set_status(app, "Previous recording still finalizing reference video...");
            return RECORD_ERROR;
        }
    }
```

- [ ] **Step 3: Stop joins a straggler finalize (one-in-flight)**

In `gui_record_stop`, replace the gate (3318-3321):

```c
    if (atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running)) {
        gui_app_set_status(app, "Finalizing previous recording...");
        return;
    }
```

with:

```c
    // One finalize in flight: if the previous one is somehow still running
    // (rare -- finalize is seconds now), wait for it before handing over.
    if (atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running)) {
        gui_app_set_status(app, "Waiting for previous finalize...");
        while (atomic_load(&s_record_stop_finalizing)) {
            thrd_sleep_ms(10);
        }
        (void)gui_record_collect_finalize_if_done();
    }
```

(`collect` joins the thread and frees `s_finalizing`; the `while` covers the window where the thread has not yet set `finalize_done`.)

- [ ] **Step 4: UI — record button reflects the new session**

`misrc_tools/misrc_gui/ui/gui_ui.c` (~3468-3478). Replace:

```c
        bool record_finalizing = gui_record_is_finalizing();
        Color record_color = record_finalizing ? (Color){184, 118, 20, 255} : (app->is_recording ? COLOR_CLIP_RED : COLOR_BUTTON);
        const char *record_label = record_finalizing ? "Finalize" : (app->is_recording ? "Stop Rec" : "Record");
```

with:

```c
        bool record_finalizing = gui_record_is_finalizing();
        // Finalize no longer blocks a new recording; tint the idle button
        // orange as the indicator, but recording state always wins.
        Color record_color = app->is_recording ? COLOR_CLIP_RED
                             : (record_finalizing ? (Color){184, 118, 20, 255} : COLOR_BUTTON);
        const char *record_label = app->is_recording ? "Stop Rec"
                                   : (record_finalizing ? "Finalize" : "Record");
```

and gate the write-error blink so it never masks an active recording:

```c
        if (record_finalizing && !app->is_recording && gui_record_has_write_error()) {
```

Click handler (~5901-5908). Replace:

```c
            if (app->is_capturing) {
                if (gui_record_is_finalizing()) {
                    gui_app_set_status(app, "Finalizing previous recording...");
                } else if (app->is_recording) {
```

with:

```c
            if (app->is_capturing) {
                if (app->is_recording) {
```

(the `else` branch calling `gui_app_start_recording(app)` stays; `gui_record_start` itself refuses on collision).

- [ ] **Step 5: Build + smoke-run + commit**

```bash
meson compile -C build-fedora misrc_gui
./build-fedora/misrc_gui & sleep 5; kill %1
git add misrc_tools/misrc_gui/output/gui_record.c misrc_tools/misrc_gui/ui/gui_ui.c
git commit -m "feat(gui): allow starting a new capture while previous finalizes"
```

---

### Task 7: Shutdown safety — window holds during finalize, cleanup joins

**Files:**
- Modify: `misrc_tools/misrc_gui/core/misrc_gui.c` (main loop ~line 573, teardown ~829-843)

**Interfaces:**
- Consumes: `gui_record_is_finalizing()`, `gui_record_cleanup()` (both already in `gui_record.h`).
- Produces: none (behavioral).

- [ ] **Step 1: Hold the window open while finalizing**

Replace the main loop head (line 573):

```c
    while (!WindowShouldClose() && !atomic_load(&do_exit)) {
```

with:

```c
    while (!atomic_load(&do_exit)) {
        if (WindowShouldClose()) {
            /* Never abandon an in-flight finalize: closing mid-rewrite leaves
             * output files with incomplete metadata (seen 2026-08-18). Hold
             * the window until finalize completes, then fall through. */
            if (gui_record_is_finalizing()) {
                gui_app_set_status(&app, "Finalizing recording -- please wait...");
            } else {
                break;
            }
        }
```

(raylib latches the close request, so once finalize completes the next `WindowShouldClose()` still returns true and the loop breaks.)

- [ ] **Step 2: Join finalize in teardown**

In the teardown block (~829-843), after the `if (app.is_capturing) { gui_app_stop_capture(&app); }` block and before `gui_settings_save(&app.settings);`, add:

```c
    /* Joins any in-flight finalize thread and frees its session. This is the
     * backstop for exit paths that bypass the main loop's finalize hold. */
    gui_record_cleanup();
```

(`gui_app_stop_recording` above it starts the finalize for a still-running recording; `gui_record_cleanup` waits for it. This is the first-ever caller of `gui_record_cleanup` — the never-called bug from the spec.)

- [ ] **Step 3: Build + smoke-run + commit**

```bash
meson compile -C build-fedora misrc_gui
./build-fedora/misrc_gui & sleep 5; kill %1
git add misrc_tools/misrc_gui/core/misrc_gui.c
git commit -m "fix(gui): never abandon finalize on exit; hold window while finalizing"
```

---

### Task 8: Full verification and PR

**Files:** none new.

- [ ] **Step 1: Full CI guard suite**

```bash
cd /home/rdodge/Repos/MISRC-GUI && python3 misrc_tools/test/ci_guard_tests.py
```
Expected: every check PASS, including the extended FLAC runtime harness.

- [ ] **Step 2: Full build**

```bash
meson compile -C build-fedora misrc_gui misrc_capture misrc_extract
```
Expected: success.

- [ ] **Step 3: Simulated-device end-to-end check**

Launch `./build-fedora/misrc_gui`, select the simulated device, start capture, record ~10 s to a scratch dir, stop, and immediately hit Record again with a *different* base name (expect: starts instantly, no "Finalizing previous recording..." refusal). Then stop and close the window during finalize of a recording (expect: "Finalizing recording -- please wait..." then clean exit). Verify the output FLAC:

```bash
metaflac --list <scratch>/<name>_videoRF_8-bit.flac | grep -E "type:|comment\[|length:"
```
Expected blocks: STREAMINFO, VORBIS_COMMENT with the 5 duration tags, SEEKTABLE, PADDING (length 4096 minus the ~178 bytes absorbed by the tags — i.e. roughly 3910-3918). No `.metadata_edit` file left behind.

- [ ] **Step 4: Push and open the PR**

```bash
git remote -v   # identify the GDH remote BY URL (git@github.com:GDH-Technologies/MISRC-GUI.git)
git push <gdh-remote-name> fix/decoupled-finalize
gh pr create --repo GDH-Technologies/MISRC-GUI --base main \
  --title "Decouple capture finalization; in-place FLAC tag embedding" \
  --body "$(cat <<'EOF'
Implements docs/superpowers/specs/2026-08-18-decoupled-finalize-design.md:

- flac_writer reserves a 4 KiB PADDING block; new flac_writer_embed_tags()
  writes vorbis tags in place (chain API) instead of rewriting the whole
  file (~10-20 min on full-tape captures previously)
- gui_record.c per-recording state moves into gui_record_session_t; a new
  capture can be configured and started while the previous one finalizes
  (one finalize in flight; path/video collision guards)
- gui_record_cleanup() is finally wired into shutdown, and the window holds
  open during an in-flight finalize -- closing the app can no longer
  truncate finalization (root cause of the 2026-08-18 interrupted capture)
- CI guard harness extended: padding presence, in-place embed, legacy
  fallback, tag round-trip

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 5: Hardware pass (operator — flag for Reece)**

With the MISRC attached: short real capture → stop → immediately start a second capture; close the app during a finalize. Confirm tags via `metaflac --list` and that hifi-decode reads a padded capture with no WARN.
