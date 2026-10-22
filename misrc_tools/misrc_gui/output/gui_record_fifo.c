/*
 * MISRC GUI - ordered record FIFO (see gui_record_fifo.h for the contract)
 */

#if !defined(_WIN32) && !defined(_WIN64) && !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  /* fallocate + FALLOC_FL_* (glibc, bionic) */
#endif

#include "gui_record_fifo.h"

#include "../../common/threading.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "../../common/file_utils.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/vfs.h>
#endif
#endif

#define GUI_RECORD_FIFO_DEFAULT_READ   ((size_t)2 * 1024 * 1024)
#define GUI_RECORD_FIFO_DEFAULT_H      ((size_t)8 * 1024 * 1024)
#define GUI_RECORD_FIFO_LOG_STEP       ((uint64_t)256 * 1024 * 1024)
#define GUI_RECORD_FIFO_PUNCH_STEP     ((uint64_t)64 * 1024 * 1024)
#define GUI_RECORD_FIFO_TRUNC_DEFAULT  ((uint64_t)256 * 1024 * 1024)
#define GUI_RECORD_FIFO_EPISODE_IDLE_MS 2000u

/* ---- gate ----------------------------------------------------------------- */

void gui_record_gate_enable(gui_record_gate_t *g) {
    if (g) atomic_store(&g->enabled, true);
}

void gui_record_gate_disable(gui_record_gate_t *g) {
    if (g) atomic_store(&g->enabled, false);
}

bool gui_record_gate_enter(gui_record_gate_t *g) {
    if (!g) return false;
    atomic_fetch_add(&g->inflight, 1);
    return atomic_load(&g->enabled);
}

void gui_record_gate_leave(gui_record_gate_t *g) {
    if (g) atomic_fetch_sub(&g->inflight, 1);
}

void gui_record_gate_wait_idle(gui_record_gate_t *g) {
    if (!g) return;
    while (atomic_load(&g->inflight) != 0) {
        thrd_sleep_ms(1);
    }
}

/* ---- logging -------------------------------------------------------------- */

static void fifo_log(gui_record_fifo_t *f, const char *level, const char *fmt, ...) {
    if (!f || !f->cfg.log) return;
    char msg[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    f->cfg.log(f->cfg.log_user, f->cfg.channel, level, msg);
}

static char fifo_ch(const gui_record_fifo_t *f) {
    return f->cfg.channel == 0 ? 'A' : 'B';
}

static double fifo_mib(uint64_t bytes) {
    return (double)bytes / (1024.0 * 1024.0);
}

/* "rf_sample=N (t=S.SSSs)" for a stream byte position. */
static void fifo_where(const gui_record_fifo_t *f, uint64_t seq, char *out, size_t n) {
    uint64_t sample = seq / f->cfg.sample_bytes;
    if (f->cfg.sample_rate_hz) {
        snprintf(out, n, "rf_sample=%llu (t=%.3fs)", (unsigned long long)sample,
                 (double)sample / (double)f->cfg.sample_rate_hz);
    } else {
        snprintf(out, n, "rf_sample=%llu", (unsigned long long)sample);
    }
}

/* ---- filesystem kind ------------------------------------------------------ */

gui_record_fs_kind_t gui_record_fifo_fs_kind(const char *path, char *name, size_t name_size) {
    if (name && name_size) name[0] = '\0';
    if (!path || !path[0]) return GUI_RECORD_FS_UNKNOWN;
#if defined(_WIN32) || defined(_WIN64)
    if ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/')) {
        if (name && name_size) snprintf(name, name_size, "unc");
        return GUI_RECORD_FS_NETWORK;
    }
    wchar_t *wide = file_utf8_to_wide(path);
    if (!wide) return GUI_RECORD_FS_UNKNOWN;
    wchar_t root[MAX_PATH + 1];
    BOOL got = GetVolumePathNameW(wide, root, MAX_PATH);
    free(wide);
    if (!got) return GUI_RECORD_FS_UNKNOWN;
    UINT type = GetDriveTypeW(root);
    switch (type) {
        case DRIVE_REMOTE:
            if (name && name_size) snprintf(name, name_size, "remote");
            return GUI_RECORD_FS_NETWORK;
        case DRIVE_RAMDISK:
            if (name && name_size) snprintf(name, name_size, "ramdisk");
            return GUI_RECORD_FS_RAM;
        case DRIVE_FIXED:
        case DRIVE_REMOVABLE:
            if (name && name_size) snprintf(name, name_size, "%s", type == DRIVE_FIXED ? "fixed" : "removable");
            return GUI_RECORD_FS_LOCAL;
        default:
            if (name && name_size) snprintf(name, name_size, "drive-type-%u", (unsigned)type);
            return GUI_RECORD_FS_UNKNOWN;
    }
#elif defined(__APPLE__)
    struct statfs st;
    if (statfs(path, &st) != 0) return GUI_RECORD_FS_UNKNOWN;
    if (name && name_size) snprintf(name, name_size, "%s", st.f_fstypename);
    if (strcmp(st.f_fstypename, "nfs") == 0 || strcmp(st.f_fstypename, "smbfs") == 0 ||
        strcmp(st.f_fstypename, "afpfs") == 0 || strcmp(st.f_fstypename, "webdav") == 0 ||
        strcmp(st.f_fstypename, "cifs") == 0 || strcmp(st.f_fstypename, "ftp") == 0 ||
        strncmp(st.f_fstypename, "osxfuse", 7) == 0 || strncmp(st.f_fstypename, "macfuse", 7) == 0) {
        return GUI_RECORD_FS_NETWORK;
    }
    return (st.f_flags & MNT_LOCAL) ? GUI_RECORD_FS_LOCAL : GUI_RECORD_FS_NETWORK;
#else
    struct statfs st;
    if (statfs(path, &st) != 0) return GUI_RECORD_FS_UNKNOWN;
    static const struct { unsigned long magic; const char *name; gui_record_fs_kind_t kind; } k[] = {
        { 0x6969UL,     "nfs",      GUI_RECORD_FS_NETWORK },
        { 0x517BUL,     "smb",      GUI_RECORD_FS_NETWORK },
        { 0xFE534D42UL, "smb2",     GUI_RECORD_FS_NETWORK },
        { 0xFF534D42UL, "cifs",     GUI_RECORD_FS_NETWORK },
        { 0x65735546UL, "fuse",     GUI_RECORD_FS_NETWORK },
        { 0x01021997UL, "9p",       GUI_RECORD_FS_NETWORK },
        { 0x00C36400UL, "ceph",     GUI_RECORD_FS_NETWORK },
        { 0x5346414FUL, "afs",      GUI_RECORD_FS_NETWORK },
        { 0x01021994UL, "tmpfs",    GUI_RECORD_FS_RAM },
        { 0x858458F6UL, "ramfs",    GUI_RECORD_FS_RAM },
        { 0x9123683EUL, "btrfs",    GUI_RECORD_FS_LOCAL },
        { 0xEF53UL,     "ext4",     GUI_RECORD_FS_LOCAL },
        { 0x58465342UL, "xfs",      GUI_RECORD_FS_LOCAL },
        { 0xF2F52010UL, "f2fs",     GUI_RECORD_FS_LOCAL },
        { 0x2FC12FC1UL, "zfs",      GUI_RECORD_FS_LOCAL },
        { 0x2011BAB0UL, "exfat",    GUI_RECORD_FS_LOCAL },
        { 0x4D44UL,     "vfat",     GUI_RECORD_FS_LOCAL },
        { 0x5346544EUL, "ntfs",     GUI_RECORD_FS_LOCAL },
        { 0x7366746EUL, "ntfs3",    GUI_RECORD_FS_LOCAL },
        { 0x794C7630UL, "overlay",  GUI_RECORD_FS_LOCAL },
        { 0x3153464AUL, "jfs",      GUI_RECORD_FS_LOCAL },
        { 0x52654973UL, "reiserfs", GUI_RECORD_FS_LOCAL },
        { 0xCA451A4EUL, "bcachefs", GUI_RECORD_FS_LOCAL },
    };
    unsigned long magic = (unsigned long)(uint32_t)st.f_type;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (k[i].magic == magic) {
            if (name && name_size) snprintf(name, name_size, "%s", k[i].name);
            return k[i].kind;
        }
    }
    if (name && name_size) snprintf(name, name_size, "0x%lX", magic);
    return GUI_RECORD_FS_UNKNOWN;
#endif
}

/* ---- spill file ----------------------------------------------------------- */

#if defined(_WIN32) || defined(_WIN64)

static bool spill_open(gui_record_spill_file_t *s, const char *dir, int channel,
                       char *err, size_t err_size) {
    static atomic_uint s_seq = 0;
    for (int attempt = 0; attempt < 16; attempt++) {
        snprintf(s->path, sizeof(s->path), "%s\\.misrc_record_spill_%c_%lu_%u.tmp", dir,
                 channel == 0 ? 'A' : 'B', (unsigned long)GetCurrentProcessId(),
                 (unsigned)atomic_fetch_add(&s_seq, 1));
        wchar_t *wide = file_utf8_to_wide(s->path);
        if (!wide) break;
        HANDLE h = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                               NULL);
        free(wide);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            break;
        }
        /* A second file object for the reader, so its ReadFile never
         * serializes behind the producer's WriteFile on one handle. */
        HANDLE r = ReOpenFile(h, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        s->wh = h;
        s->rh = (r != INVALID_HANDLE_VALUE) ? r : NULL;
        s->open = true;
        return true;
    }
    if (err && err_size) {
        snprintf(err, err_size, "cannot create a spill file in %s (error %lu)", dir,
                 (unsigned long)GetLastError());
    }
    return false;
}

static bool spill_io(void *handle, void *buf, size_t len, uint64_t off, bool write) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        DWORD chunk = (len > 0x40000000u) ? 0x40000000u : (DWORD)len;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD done = 0;
        BOOL ok = write ? WriteFile((HANDLE)handle, p, chunk, &done, &ov)
                        : ReadFile((HANDLE)handle, p, chunk, &done, &ov);
        if (!ok || done == 0) return false;
        p += done;
        len -= done;
        off += done;
    }
    return true;
}

static bool spill_write(gui_record_spill_file_t *s, const void *buf, size_t len, uint64_t off) {
    return spill_io(s->wh, (void *)buf, len, off, true);
}

static bool spill_read(gui_record_spill_file_t *s, void *buf, size_t len, uint64_t off) {
    return spill_io(s->rh ? s->rh : s->wh, buf, len, off, false);
}

static bool spill_truncate(gui_record_spill_file_t *s) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return SetFilePointerEx((HANDLE)s->wh, zero, NULL, FILE_BEGIN) && SetEndOfFile((HANDLE)s->wh);
}

static bool spill_punch(gui_record_spill_file_t *s, uint64_t off, uint64_t len) {
    (void)s; (void)off; (void)len;
    return false;  /* not attempted: the drained file is reused instead */
}

static void spill_close(gui_record_spill_file_t *s) {
    if (s->rh) CloseHandle((HANDLE)s->rh);
    if (s->wh) CloseHandle((HANDLE)s->wh);
    s->rh = s->wh = NULL;
    s->open = false;
}

#else

static bool spill_open(gui_record_spill_file_t *s, const char *dir, int channel,
                       char *err, size_t err_size) {
    char tmpl[1024];
    snprintf(tmpl, sizeof(tmpl), "%s/.misrc_record_spill_%c_XXXXXX", dir, channel == 0 ? 'A' : 'B');
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        if (err && err_size) {
            snprintf(err, err_size, "cannot create a spill file in %s: %s", dir, strerror(errno));
        }
        return false;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* Unlinked at once: the space is reclaimed when the fd closes, however
     * the process ends, and nothing is left for anyone to trip over. */
    (void)unlink(tmpl);
    s->fd = fd;
    s->open = true;
    snprintf(s->path, sizeof(s->path), "%s (unlinked)", tmpl);
    return true;
}

static bool spill_write(gui_record_spill_file_t *s, const void *buf, size_t len, uint64_t off) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = pwrite(s->fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return true;
}

static bool spill_read(gui_record_spill_file_t *s, void *buf, size_t len, uint64_t off) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = pread(s->fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  /* short file: the bytes are not there */
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return true;
}

static bool spill_truncate(gui_record_spill_file_t *s) {
    return ftruncate(s->fd, 0) == 0;
}

static bool spill_punch(gui_record_spill_file_t *s, uint64_t off, uint64_t len) {
#if defined(__APPLE__) && defined(F_PUNCHHOLE)
    fpunchhole_t ph;
    memset(&ph, 0, sizeof(ph));
    ph.fp_offset = (off_t)off;
    ph.fp_length = (off_t)len;
    return fcntl(s->fd, F_PUNCHHOLE, &ph) == 0;
#elif defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    return fallocate(s->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)len) == 0;
#else
    (void)s; (void)off; (void)len;
    return false;
#endif
}

static void spill_close(gui_record_spill_file_t *s) {
    if (s->open) close(s->fd);
    s->fd = -1;
    s->open = false;
}

#endif

/* ---- setup ---------------------------------------------------------------- */

static uint32_t fifo_next_pow2(uint64_t v) {
    uint32_t p = 64;
    while (p < v && p < (1u << 24)) p <<= 1;
    return p;
}

int gui_record_fifo_init(gui_record_fifo_t *f, const gui_record_fifo_config_t *cfg,
                         char *err, size_t err_size) {
    if (err && err_size) err[0] = '\0';
    if (!f || !cfg || !cfg->bufmgr || cfg->sample_bytes == 0) {
        if (err && err_size) snprintf(err, err_size, "invalid record FIFO configuration");
        return -1;
    }
    memset(f, 0, sizeof(*f));
    f->cfg = *cfg;
#if !defined(_WIN32) && !defined(_WIN64)
    f->spill.fd = -1;
#endif

    /* Initialize the ring here, on the record layer's thread, so the
     * capture-side producer never pays for a lazy mmap of up to 8 GiB. */
    if (bufmgr_ensure_init(cfg->bufmgr, cfg->buf_id) < 0) {
        if (err && err_size) snprintf(err, err_size, "record ring %d could not be allocated", (int)cfg->buf_id);
        return -1;
    }
    f->ring_size = cfg->bufmgr->buffers[cfg->buf_id].buffer_size;
    if (bufmgr_fill_level(cfg->bufmgr, cfg->buf_id) != 0) {
        /* The segment log starts at stream byte 0 in the ring; stale bytes
         * would be read as the first segment. */
        bufmgr_reset(cfg->bufmgr, cfg->buf_id);
    }

    size_t sb = cfg->sample_bytes;
    size_t max_push = cfg->max_push_bytes ? cfg->max_push_bytes : (size_t)128 * 1024;
    f->max_read = cfg->max_read_bytes ? cfg->max_read_bytes : GUI_RECORD_FIFO_DEFAULT_READ;
    if (f->max_read > f->ring_size / 2) f->max_read = f->ring_size / 2;
    f->max_read -= f->max_read % sb;
    if (f->max_read < sb) f->max_read = sb;

    size_t H = cfg->switch_back_free_bytes;
    if (H == 0) {
        H = GUI_RECORD_FIFO_DEFAULT_H;
        if (H < 4 * max_push) H = 4 * max_push;
        if (H > f->ring_size / 4) H = f->ring_size / 4;
    }
    if (H < max_push) H = max_push;
    f->H = H;

    if (cfg->seg_cap_override) {
        f->seg_cap = cfg->seg_cap_override;
    } else {
        /* Unconsumed RING segments all sit in the ring at once and each holds
         * at least H - max_push bytes, so at most ring/(H - max_push) + 1 of
         * them exist; SPILL segments alternate with them. */
        uint64_t per = (H > max_push) ? (uint64_t)(H - max_push) : 1;
        f->seg_cap = fifo_next_pow2(2 * (f->ring_size / per) + 8);
    }
    f->segs = (gui_record_fifo_seg_t *)calloc(f->seg_cap, sizeof(*f->segs));
    f->c_buf = (uint8_t *)malloc(f->max_read);
    if (!f->segs || !f->c_buf) {
        free(f->segs);
        free(f->c_buf);
        f->segs = NULL;
        f->c_buf = NULL;
        if (err && err_size) snprintf(err, err_size, "out of memory for the record FIFO");
        return -1;
    }
    f->truncate_threshold = cfg->truncate_threshold ? cfg->truncate_threshold : GUI_RECORD_FIFO_TRUNC_DEFAULT;

    /* Segment 0: the stream starts in the ring. */
    f->segs[0].src = GUI_RECORD_SEG_RING;
    f->segs[0].start_seq = 0;
    f->segs[0].file_off = 0;
    atomic_store(&f->seg_tail, 1);
    atomic_store(&f->seg_head, 0);
    atomic_store(&f->pub_seq, 0);
    atomic_store(&f->spill_produced, 0);
    atomic_store(&f->spill_consumed, 0);
    atomic_store(&f->cons_seq, 0);
    atomic_store(&f->closed, false);
    atomic_store(&f->punch_ok, false);
    f->p_src = GUI_RECORD_SEG_RING;
    f->p_log_mark = GUI_RECORD_FIFO_LOG_STEP;

    if (cfg->spill_dir && cfg->spill_dir[0]) {
        char serr[256] = {0};
        if (spill_open(&f->spill, cfg->spill_dir, cfg->channel, serr, sizeof(serr))) {
            f->spill_ok = true;
            /* Probe hole punching once: write 64 KiB, punch it, truncate. */
            if (!cfg->force_no_punch) {
                static const uint8_t zeros[65536];
                if (spill_write(&f->spill, zeros, sizeof(zeros), 0) &&
                    spill_punch(&f->spill, 0, sizeof(zeros))) {
                    atomic_store(&f->punch_ok, true);
                }
                (void)spill_truncate(&f->spill);
            }
        } else if (err && err_size) {
            snprintf(err, err_size, "%s", serr);
        }
    }
    return 0;
}

void gui_record_fifo_destroy(gui_record_fifo_t *f) {
    if (!f) return;
    spill_close(&f->spill);
    free(f->segs);
    free(f->c_buf);
    f->segs = NULL;
    f->c_buf = NULL;
    f->spill_ok = false;
}

bool gui_record_fifo_spill_available(const gui_record_fifo_t *f) {
    return f && f->spill_ok;
}

const char *gui_record_fifo_spill_path(const gui_record_fifo_t *f) {
    return (f && f->spill_ok) ? f->spill.path : "(none)";
}

/* ---- producer ------------------------------------------------------------- */

static uint64_t fifo_seg_room(gui_record_fifo_t *f) {
    uint64_t tail = atomic_load_explicit(&f->seg_tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&f->seg_head, memory_order_acquire);
    return (uint64_t)f->seg_cap - (tail - head);
}

/* Publish a segment boundary before the segment's first byte. */
static void fifo_seg_append(gui_record_fifo_t *f, uint32_t src, uint64_t file_off) {
    uint64_t tail = atomic_load_explicit(&f->seg_tail, memory_order_relaxed);
    gui_record_fifo_seg_t *s = &f->segs[tail & (f->seg_cap - 1)];
    s->src = src;
    s->start_seq = f->p_seq;
    s->file_off = file_off;
    atomic_store_explicit(&f->seg_tail, tail + 1, memory_order_release);
    f->p_src = src;
    atomic_fetch_add(&f->st_segments, 1);
}

static void fifo_publish(gui_record_fifo_t *f, size_t len) {
    f->p_seq += len;
    atomic_store_explicit(&f->pub_seq, f->p_seq, memory_order_release);
}

static gui_record_push_result_t fifo_drop(gui_record_fifo_t *f, size_t len, uint32_t frame,
                                          const char *reason) {
    atomic_fetch_add(&f->st_drop_pushes, 1);
    atomic_fetch_add(&f->st_drop_bytes, len);
    atomic_fetch_add(&f->cfg.bufmgr->stats[f->cfg.buf_id].write_drops, 1);
    f->ep_drops++;
    uint64_t n = ++f->drop_log_count;
    if (n <= 5 || n % 1000 == 0) {
        char where[96];
        fifo_where(f, f->p_seq, where, sizeof(where));
        fifo_log(f, "ERROR",
                 "Record data loss on channel %c at %s, frame=%u: %s; %llu push(es), %.2f MiB lost so far",
                 fifo_ch(f), where, (unsigned)frame, reason,
                 (unsigned long long)atomic_load(&f->st_drop_pushes),
                 fifo_mib(atomic_load(&f->st_drop_bytes)));
    }
    return GUI_RECORD_PUSH_DROPPED;
}

static void fifo_episode_begin(gui_record_fifo_t *f, uint64_t now_ms, uint32_t frame) {
    f->ep_segs++;
    if (f->ep_open) return;
    f->ep_open = true;
    f->ep_start_seq = f->p_seq;
    f->ep_end_seq = f->p_seq;
    f->ep_start_ms = now_ms;
    f->ep_last_spill_ms = now_ms;
    f->ep_spilled = 0;
    f->ep_peak = 0;
    f->ep_drops = 0;
    f->ep_segs = 1;
    f->p_log_mark = GUI_RECORD_FIFO_LOG_STEP;
    uint64_t ep = atomic_fetch_add(&f->st_episodes, 1) + 1;
    char where[96];
    fifo_where(f, f->p_seq, where, sizeof(where));
    fifo_log(f, "WARN",
             "Record overflow on channel %c: RAM buffer full at %s, frame=%u; "
             "continuing on the disk spill, in order (episode %llu, spill %s)",
             fifo_ch(f), where, (unsigned)frame, (unsigned long long)ep, f->spill.path);
}

void gui_record_fifo_maybe_end_episode(gui_record_fifo_t *f, uint64_t now_ms, uint64_t idle_ms) {
    if (!f || !f->ep_open || f->p_src != GUI_RECORD_SEG_RING) return;
    if (now_ms - f->ep_last_spill_ms < idle_ms) return;
    if (gui_record_fifo_spill_backlog(f) != 0) return;
    char a[96], b[96];
    fifo_where(f, f->ep_start_seq, a, sizeof(a));
    fifo_where(f, f->ep_end_seq, b, sizeof(b));
    fifo_log(f, "INFO",
             "Record overflow on channel %c ended: %s .. %s went through the disk spill in order; "
             "spilled %.2f MiB in %u segment(s), peak backlog %.2f MiB, %llu drop(s), %.1fs",
             fifo_ch(f), a, b, fifo_mib(f->ep_spilled), (unsigned)f->ep_segs, fifo_mib(f->ep_peak),
             (unsigned long long)f->ep_drops, (double)(now_ms - f->ep_start_ms) / 1000.0);
    f->ep_open = false;
}

/* Recycle mode only: give a large, fully drained spill file back. The reader
 * reads only [consumed, produced) of the spill, which is empty here, and the
 * producer is the file's only writer, so nothing can be in flight. */
static void fifo_maybe_truncate(gui_record_fifo_t *f) {
    if (!f->spill_ok || atomic_load_explicit(&f->punch_ok, memory_order_relaxed)) return;
    if (f->p_spill_hw <= f->truncate_threshold) return;
    if (atomic_load_explicit(&f->spill_consumed, memory_order_acquire) != f->p_spill_produced) return;
    if (spill_truncate(&f->spill)) {
        f->p_spill_hw = 0;
        f->p_spill_cursor = 0;
    }
}

static gui_record_push_result_t fifo_ring_write(gui_record_fifo_t *f, const void *bytes,
                                                size_t len, uint64_t now_ms) {
    void *dst = bufmgr_try_write_begin(f->cfg.bufmgr, f->cfg.buf_id, len);
    if (!dst) return GUI_RECORD_PUSH_IDLE;
    memcpy(dst, bytes, len);
    if (!bufmgr_write_commit(f->cfg.bufmgr, f->cfg.buf_id, len)) {
        /* Cannot happen with a single producer (the space was there and only
         * the reader changes it, by freeing more); never publish bytes the
         * ring does not hold. */
        return GUI_RECORD_PUSH_IDLE;
    }
    fifo_publish(f, len);
    atomic_fetch_add(&f->st_ring_bytes, len);
    gui_record_fifo_maybe_end_episode(f, now_ms, GUI_RECORD_FIFO_EPISODE_IDLE_MS);
    fifo_maybe_truncate(f);
    return GUI_RECORD_PUSH_RING;
}

gui_record_push_result_t gui_record_fifo_push(gui_record_fifo_t *f, const void *bytes,
                                              size_t len, uint32_t frame_index) {
    if (!f || !bytes || len == 0) return GUI_RECORD_PUSH_IDLE;
    if (len % f->cfg.sample_bytes != 0) {
        return fifo_drop(f, len, frame_index, "push is not a whole number of samples");
    }
    uint64_t now_ms = get_time_ms();

    /* Hysteresis: leave the spill only once the ring has real room again,
     * so a ring hovering at full does not cut the stream into tiny
     * segments. */
    if (f->p_src == GUI_RECORD_SEG_SPILL &&
        bufmgr_free_space(f->cfg.bufmgr, f->cfg.buf_id) >= f->H + len &&
        fifo_seg_room(f) >= 1) {
        fifo_seg_append(f, GUI_RECORD_SEG_RING, 0);
    }

    if (f->p_src == GUI_RECORD_SEG_RING) {
        gui_record_push_result_t r = fifo_ring_write(f, bytes, len, now_ms);
        if (r == GUI_RECORD_PUSH_RING) return r;
        /* Ring full: open a SPILL segment. */
        if (!f->spill_ok) {
            return fifo_drop(f, len, frame_index, "RAM buffer full and no spill file");
        }
        if (fifo_seg_room(f) < 1) {
            return fifo_drop(f, len, frame_index, "RAM buffer full and the segment log is full");
        }
        uint64_t off = f->p_spill_cursor;
        if (!atomic_load_explicit(&f->punch_ok, memory_order_relaxed) &&
            atomic_load_explicit(&f->spill_consumed, memory_order_acquire) == f->p_spill_produced &&
            off != 0) {
            /* Fully drained: reuse the file from the start. */
            off = 0;
            atomic_fetch_add(&f->st_recycles, 1);
        }
        f->p_spill_cursor = off;
        fifo_seg_append(f, GUI_RECORD_SEG_SPILL, off);
        fifo_episode_begin(f, now_ms, frame_index);
    }

    /* SPILL segment: append to the file. */
    bool wrote = f->cfg.spill_write_hook
        ? f->cfg.spill_write_hook(f->cfg.hook_user, bytes, len, f->p_spill_cursor)
        : spill_write(&f->spill, bytes, len, f->p_spill_cursor);
    if (wrote && f->cfg.spill_write_hook) {
        wrote = spill_write(&f->spill, bytes, len, f->p_spill_cursor);
    }
    if (wrote) {
        f->p_spill_cursor += len;
        if (f->p_spill_cursor > f->p_spill_hw) f->p_spill_hw = f->p_spill_cursor;
        f->p_spill_produced += len;
        atomic_store_explicit(&f->spill_produced, f->p_spill_produced, memory_order_release);
        fifo_publish(f, len);
        bufmgr_signal_data(f->cfg.bufmgr, f->cfg.buf_id);
        atomic_fetch_add(&f->st_spill_bytes, len);
        atomic_fetch_add(&f->st_spill_pushes, 1);
        atomic_fetch_add(&f->cfg.bufmgr->stats[f->cfg.buf_id].write_waits, 1);
        f->ep_spilled += len;
        f->ep_last_spill_ms = now_ms;
        f->ep_end_seq = f->p_seq;
        uint64_t backlog = gui_record_fifo_spill_backlog(f);
        if (backlog > f->ep_peak) f->ep_peak = backlog;
        uint64_t peak = atomic_load(&f->st_max_backlog);
        while (backlog > peak && !atomic_compare_exchange_weak(&f->st_max_backlog, &peak, backlog)) {
        }
        if (backlog >= f->p_log_mark) {
            fifo_log(f, "WARN", "Record overflow on channel %c: spill backlog %.2f MiB",
                     fifo_ch(f), fifo_mib(backlog));
            while (f->p_log_mark <= backlog) f->p_log_mark += GUI_RECORD_FIFO_LOG_STEP;
        }
        return GUI_RECORD_PUSH_SPILL;
    }

    uint64_t errs = atomic_fetch_add(&f->st_spill_write_errors, 1) + 1;
    if (errs <= 3 || errs % 1000 == 0) {
        fifo_log(f, "ERROR", "Record spill write failed on channel %c (%s): %s",
                 fifo_ch(f), f->spill.path, strerror(errno));
    }
    /* The ring may have room by now; a new RING segment keeps order. */
    if (fifo_seg_room(f) >= 1 && bufmgr_free_space(f->cfg.bufmgr, f->cfg.buf_id) >= len) {
        fifo_seg_append(f, GUI_RECORD_SEG_RING, 0);
        gui_record_push_result_t r = fifo_ring_write(f, bytes, len, now_ms);
        if (r == GUI_RECORD_PUSH_RING) return r;
    }
    return fifo_drop(f, len, frame_index, "RAM buffer full and the spill write failed");
}

void gui_record_fifo_close(gui_record_fifo_t *f) {
    if (!f) return;
    atomic_store_explicit(&f->closed, true, memory_order_release);
    bufmgr_signal_data(f->cfg.bufmgr, f->cfg.buf_id);
}

/* ---- reader --------------------------------------------------------------- */

static void fifo_wait(gui_record_fifo_t *f, uint64_t ms) {
    if (ms == 0) return;
    uint64_t t0 = get_time_ms();
    (void)bufmgr_wait_data(f->cfg.bufmgr, f->cfg.buf_id, (int)ms);
    if (get_time_ms() == t0) thrd_sleep_ms(1);  /* no events: do not spin */
}

int gui_record_fifo_next(gui_record_fifo_t *f, size_t max_bytes, int timeout_ms,
                         gui_record_fifo_chunk_t *chunk) {
    if (!f || !chunk) return -1;
    size_t cap = (max_bytes && max_bytes < f->max_read) ? max_bytes : f->max_read;
    cap -= cap % f->cfg.sample_bytes;
    if (cap == 0) cap = f->cfg.sample_bytes;
    uint64_t start = get_time_ms();
    uint64_t limit = timeout_ms > 0 ? (uint64_t)timeout_ms : 0;
    uint32_t ring_misses = 0;

    for (;;) {
        /* Order matters. closed before pub_seq: once closed is seen, pub_seq
         * is final. pub_seq before seg_tail: a segment entry is released
         * before its first byte is published, so every segment holding a
         * byte below P is visible in T, and "end = P" is used only when no
         * later segment can own those bytes. Loading seg_tail first could
         * read ring bytes that belong to a later RING segment while an
         * earlier SPILL segment is still unread -- a reorder. */
        bool closed = atomic_load_explicit(&f->closed, memory_order_acquire);
        uint64_t P = atomic_load_explicit(&f->pub_seq, memory_order_acquire);
        uint64_t T = atomic_load_explicit(&f->seg_tail, memory_order_acquire);
        const gui_record_fifo_seg_t *s = &f->segs[f->c_seg & (f->seg_cap - 1)];
        bool has_next = f->c_seg + 1 < T;
        uint64_t end = has_next ? f->segs[(f->c_seg + 1) & (f->seg_cap - 1)].start_seq : P;
        uint64_t elapsed = get_time_ms() - start;
        uint64_t left = elapsed < limit ? limit - elapsed : 0;

        if (f->c_seq >= end) {
            if (has_next) {
                f->c_seg++;
                atomic_store_explicit(&f->seg_head, f->c_seg, memory_order_release);
                continue;
            }
            if (closed) return -1;
            if (left == 0) return 0;
            fifo_wait(f, left < 10 ? left : 10);
            continue;
        }

        uint64_t avail = end - f->c_seq;
        /* An open segment with less than a full chunk: give the producer a
         * moment so the writer handles full blocks, not a trickle. */
        if (avail < cap && !has_next && !closed && left > 0) {
            fifo_wait(f, left < 5 ? left : 5);
            continue;
        }
        size_t n = (avail < cap) ? (size_t)avail : cap;

        if (s->src == GUI_RECORD_SEG_RING) {
            void *p = bufmgr_read_begin(f->cfg.bufmgr, f->cfg.buf_id, n, 0);
            if (!p) {
                /* Published ring bytes are committed first, so this is not
                 * expected; never skip them -- wait and look again. */
                if (++ring_misses == 1) {
                    fifo_log(f, "ERROR", "Record ring on channel %c is behind its published stream; waiting",
                             fifo_ch(f));
                }
                if (left == 0) return 0;
                fifo_wait(f, 1);
                continue;
            }
            chunk->data = p;
            chunk->len = n;
            chunk->src = GUI_RECORD_SEG_RING;
        } else {
            uint64_t off = s->file_off + (f->c_seq - s->start_seq);
            bool ok = false;
            for (int attempt = 0; attempt < 4 && !ok; attempt++) {
                ok = spill_read(&f->spill, f->c_buf, n, off);
                if (!ok) thrd_sleep_ms(10);
            }
            if (!ok) {
                atomic_fetch_add(&f->st_read_lost, n);
                char where[96];
                fifo_where(f, f->c_seq, where, sizeof(where));
                fifo_log(f, "ERROR", "Record spill read failed on channel %c at %s: %s; %.2f MiB lost",
                         fifo_ch(f), where, strerror(errno), fifo_mib(n));
                f->c_out = n;
                f->c_out_src = GUI_RECORD_SEG_SPILL;
                gui_record_fifo_consume(f, n);
                continue;
            }
            chunk->data = f->c_buf;
            chunk->len = n;
            chunk->src = GUI_RECORD_SEG_SPILL;
        }
        f->c_out = n;
        f->c_out_src = chunk->src;
        return 1;
    }
}

void gui_record_fifo_consume(gui_record_fifo_t *f, size_t len) {
    if (!f || len == 0) return;
    if (len > f->c_out) len = f->c_out;
    if (f->c_out_src == GUI_RECORD_SEG_RING) {
        bufmgr_read_end(f->cfg.bufmgr, f->cfg.buf_id, len);
    } else {
        f->c_spill_consumed += len;
        /* Punch mode: file offsets only grow, so everything below the
         * reader's position is dead. Punch whole steps, before publishing
         * the consumption (the producer never writes below its cursor). */
        if (atomic_load_explicit(&f->punch_ok, memory_order_relaxed)) {
            const gui_record_fifo_seg_t *s = &f->segs[f->c_seg & (f->seg_cap - 1)];
            uint64_t pos = s->file_off + (f->c_seq + len - s->start_seq);
            uint64_t upto = pos - pos % GUI_RECORD_FIFO_PUNCH_STEP;
            if (upto > f->c_punched) {
                if (spill_punch(&f->spill, f->c_punched, upto - f->c_punched)) {
                    f->c_punched = upto;
                } else {
                    atomic_store(&f->punch_ok, false);
                    fifo_log(f, "WARN", "Record spill on channel %c: hole punching failed (%s); "
                             "reusing the file once drained instead", fifo_ch(f), strerror(errno));
                }
            }
        }
        atomic_store_explicit(&f->spill_consumed, f->c_spill_consumed, memory_order_release);
    }
    f->c_seq += len;
    atomic_store_explicit(&f->cons_seq, f->c_seq, memory_order_release);
    f->c_out -= len;
}

/* ---- queries -------------------------------------------------------------- */

uint64_t gui_record_fifo_spill_backlog(const gui_record_fifo_t *f) {
    if (!f) return 0;
    gui_record_fifo_t *m = (gui_record_fifo_t *)f;
    uint64_t consumed = atomic_load_explicit(&m->spill_consumed, memory_order_acquire);
    uint64_t produced = atomic_load_explicit(&m->spill_produced, memory_order_acquire);
    return produced >= consumed ? produced - consumed : 0;
}

uint64_t gui_record_fifo_backlog(const gui_record_fifo_t *f) {
    if (!f) return 0;
    gui_record_fifo_t *m = (gui_record_fifo_t *)f;
    uint64_t consumed = atomic_load_explicit(&m->cons_seq, memory_order_acquire);
    uint64_t pub = atomic_load_explicit(&m->pub_seq, memory_order_acquire);
    return pub >= consumed ? pub - consumed : 0;
}

void gui_record_fifo_get_stats(const gui_record_fifo_t *f, gui_record_fifo_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!f) return;
    gui_record_fifo_t *m = (gui_record_fifo_t *)f;
    out->accepted_bytes = atomic_load(&m->pub_seq);
    out->ring_bytes = atomic_load(&m->st_ring_bytes);
    out->spill_bytes = atomic_load(&m->st_spill_bytes);
    out->spill_pushes = atomic_load(&m->st_spill_pushes);
    out->dropped_bytes = atomic_load(&m->st_drop_bytes);
    out->dropped_pushes = atomic_load(&m->st_drop_pushes);
    out->consumed_bytes = atomic_load(&m->cons_seq);
    out->read_lost_bytes = atomic_load(&m->st_read_lost);
    out->segments = atomic_load(&m->st_segments);
    out->episodes = atomic_load(&m->st_episodes);
    out->max_backlog_bytes = atomic_load(&m->st_max_backlog);
    out->spill_write_errors = atomic_load(&m->st_spill_write_errors);
    out->recycles = atomic_load(&m->st_recycles);
}
