/*
 * libFLAC worker priority harness (Linux only; compiled and run by
 * ci_guard_tests.py).
 *
 * libFLAC 1.5 starts its encoder workers lazily, from inside
 * FLAC__stream_encoder_process(), a few at a time as frames queue up, on
 * whatever thread made the call -- and a new thread copies its creator's
 * scheduling policy and nice. The RF writer that calls flac_writer_process()
 * runs SCHED_FIFO, so left alone up to eight CPU-bound workers per channel run
 * SCHED_FIFO too, and with RT throttling off an encode that falls behind
 * starves everything else on the host. flac_writer must give every worker the
 * strongest *normal* class instead (SCHED_OTHER at the best nice the limits
 * allow) and leave the writer itself SCHED_FIFO once the call returns.
 *
 * The feed mirrors what libFLAC was seen to do: a sub-block call spawns
 * nothing, later calls spawn workers one or a few at a time.
 */
#include "flac_writer.h"
#include "threading.h"

#include <dirent.h>
#include <string.h>

#define HARNESS_EXIT_SKIP 2

typedef struct {
    pid_t tid;
    int policy;
    int rt_priority;
    int nice;
} task_info_t;

typedef struct {
    int want_worker_nice;
    int skip;
    int failed;
    pid_t writer_tid;
    int writer_policy_after;
    int writer_rt_after;
    task_info_t tasks[64];
    int task_count;
} run_t;

static int snapshot_tasks(task_info_t *out, int max)
{
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return -1;
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && n < max) {
        if (entry->d_name[0] == '.') continue;
        pid_t tid = (pid_t)atoi(entry->d_name);
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        out[n].tid = tid;
        out[n].policy = sched_getscheduler(tid);
        sched_getparam(tid, &sp);
        out[n].rt_priority = sp.sched_priority;
        errno = 0;
        out[n].nice = getpriority(PRIO_PROCESS, (id_t)tid);
        n++;
    }
    closedir(dir);
    return n;
}

static void *writer_main(void *arg)
{
    run_t *run = (run_t *)arg;
    run->writer_tid = (pid_t)syscall(SYS_gettid);

    struct sched_param fifo = { .sched_priority = 1 };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &fifo);
    if (rc != 0) {
        printf("SKIP: writer cannot become SCHED_FIFO 1 (%s)\n", strerror(rc));
        run->skip = 1;
        return NULL;
    }

    FILE *out = tmpfile();
    if (!out) { perror("tmpfile"); run->failed = 1; return NULL; }
    flac_writer_config_t cfg = flac_writer_default_config();
    cfg.sample_rate = 48000;
    cfg.bits_per_sample = 16;
    cfg.compression_level = 8;
    cfg.num_threads = 4;
    flac_writer_t *writer = flac_writer_create_stream(out, &cfg);
    if (!writer) { printf("FAIL: flac_writer_create_stream\n"); run->failed = 1; fclose(out); return NULL; }

    static int32_t samples[65536];
    for (int i = 0; i < 65536; i++) samples[i] = (i * 37) % 30000 - 15000;
    flac_writer_process(writer, samples, 100);
    flac_writer_process(writer, samples, 4096);
    for (int round = 0; round < 16; round++) {
        flac_writer_process(writer, samples, 65536);
    }

    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    pthread_getschedparam(pthread_self(), &run->writer_policy_after, &sp);
    run->writer_rt_after = sp.sched_priority;
    run->task_count = snapshot_tasks(run->tasks, 64);

    flac_writer_finish(writer);
    fclose(out);
    return NULL;
}

int main(void)
{
    run_t run;
    memset(&run, 0, sizeof(run));

    if (geteuid() == 0) {
        run.want_worker_nice = -20;   /* root ignores RLIMIT_NICE */
    } else {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NICE, &rl) != 0 ||
            (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < 25)) {
            printf("SKIP: hard RLIMIT_NICE < 25\n");
            return HARNESS_EXIT_SKIP;
        }
        rl.rlim_cur = 25;             /* nice floor 20 - 25 = -5 */
        if (setrlimit(RLIMIT_NICE, &rl) != 0) { perror("setrlimit NICE"); return 1; }
        run.want_worker_nice = -5;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, writer_main, &run) != 0) { perror("pthread_create"); return 1; }
    pthread_join(thread, NULL);
    if (run.skip) return HARNESS_EXIT_SKIP;
    if (run.failed) return 1;

    pid_t main_tid = (pid_t)syscall(SYS_gettid);
    int workers = 0;
    int bad = 0;
    for (int i = 0; i < run.task_count; i++) {
        const task_info_t *t = &run.tasks[i];
        if (t->tid == main_tid || t->tid == run.writer_tid) continue;
        workers++;
        if (t->policy != SCHED_OTHER || t->nice != run.want_worker_nice) {
            printf("FAIL: libFLAC worker %d runs policy %d rt %d nice %d, want SCHED_OTHER nice %d\n",
                   (int)t->tid, t->policy, t->rt_priority, t->nice, run.want_worker_nice);
            bad++;
        }
    }
    if (workers == 0) {
        printf("SKIP: libFLAC started no worker threads (built without multithreading?)\n");
        return HARNESS_EXIT_SKIP;
    }
    if (run.writer_policy_after != SCHED_FIFO || run.writer_rt_after != 1) {
        printf("FAIL: writer left at policy %d rt %d, want SCHED_FIFO 1 after processing\n",
               run.writer_policy_after, run.writer_rt_after);
        bad++;
    }
    if (bad == 0) {
        printf("PASS: %d libFLAC workers SCHED_OTHER nice %d, writer back on SCHED_FIFO 1\n",
               workers, run.want_worker_nice);
    }
    return bad == 0 ? 0 : 1;
}
