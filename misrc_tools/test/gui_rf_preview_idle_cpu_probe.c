/* Deterministic idle-accounting evidence probe. This includes the production
 * worker and reuses only the existing test DSP substitutes. GetThreadTimes is
 * replaced in this translation unit, without any production test API. The
 * injected 500 ms is accumulated idle thread CPU, not 500 ms of wall idle;
 * this probe cannot establish the user's real reconnect-delay root cause.
 *
 * Build with this source plus processing/gui_rf_preview_policy.c. The default
 * asserts that idle CPU is excluded from resumed processing debt. Compile
 * against the pre-fix worker and use --expect-before to repeat old evidence. */
#ifdef _WIN32
#include <windows.h>
#include <stdatomic.h>

#define main gui_rf_preview_existing_worker_test_main
#include "gui_rf_preview_worker_test.c"
#undef main

static atomic_uint_fast64_t injected_cpu_us;
static atomic_uint injected_clock_reads;

static BOOL WINAPI idle_probe_get_thread_times(HANDLE thread, LPFILETIME creation,
                                               LPFILETIME exit_time, LPFILETIME kernel,
                                               LPFILETIME user) {
    (void)thread;
    memset(creation, 0, sizeof(*creation));
    memset(exit_time, 0, sizeof(*exit_time));
    memset(kernel, 0, sizeof(*kernel));
    uint64_t ticks = atomic_load(&injected_cpu_us) * 10u;
    user->dwLowDateTime = (DWORD)ticks;
    user->dwHighDateTime = (DWORD)(ticks >> 32);
    atomic_fetch_add(&injected_clock_reads, 1);
    return TRUE;
}

#define GetThreadTimes idle_probe_get_thread_times
#include "../misrc_gui/processing/gui_rf_preview_worker.c"
#undef GetThreadTimes

static bool idle_probe_wait(atomic_uint *value, unsigned target, uint32_t timeout_ms) {
    uint64_t until = get_time_ms() + timeout_ms;
    while (get_time_ms() < until) {
        if (atomic_load(value) >= target) return true;
        thrd_sleep_ms(1);
    }
    return false;
}

static bool idle_probe_finish_marker(gui_rf_preview_worker_t *worker, uint64_t epoch,
                                     uint64_t clock_at_completion, int16_t marker) {
    atomic_store(&hold_process, true);
    atomic_store(&process_entered, false);
    if (!submit_marker(worker, 20000000u, 0, epoch, true, marker)) {
        atomic_store(&hold_process, false);
        return false;
    }
    uint64_t until = get_time_ms() + 3000;
    while (!atomic_load(&process_entered) && get_time_ms() < until) thrd_sleep_ms(1);
    bool entered = atomic_load(&process_entered);
    atomic_store(&injected_cpu_us, clock_at_completion);
    atomic_store(&hold_process, false);
    return entered && gui_rf_preview_worker_wait_idle(worker, 3000);
}

static void idle_probe_short_coarse_accounting(void) {
    atomic_store(&injected_cpu_us, 0);
    unsigned int reads_before = atomic_load(&injected_clock_reads);
    check(gui_rf_preview_worker_set_budget(0), "short accounting fixture is unpaced");
    gui_rf_preview_worker_t *worker = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(worker != NULL, "short accounting worker created");
    if (!worker) return;
    check(idle_probe_wait(&injected_clock_reads, reads_before + 1, 3000),
          "short accounting initial CPU baseline acquired");
    check(idle_probe_finish_marker(worker, 1, 0, 31), "first coarse block has no booked CPU yet");
    gui_rf_preview_stats_t first;
    gui_rf_preview_worker_get_stats(worker, &first);
    check(first.processing_cpu_ms == 0.0, "coarse counter may stay unchanged at block completion");
    thrd_sleep_ms(10);
    atomic_store(&injected_cpu_us, 15625);
    gui_rf_preview_worker_reset(worker);
    check(idle_probe_finish_marker(worker, 2, 15625, 33), "nearby block after short control reset");
    gui_rf_preview_stats_t second;
    gui_rf_preview_worker_get_stats(worker, &second);
    check(second.processing_cpu_ms == 15.625,
          "short source gap/control reset retains CPU booked between completions");
    check(idle_probe_finish_marker(worker, 2, 15625, 35), "next coarse block retains unchanged snapshot");
    check(idle_probe_finish_marker(worker, 2, 31250, 37), "later coarse completion books next quantum");
    gui_rf_preview_stats_t last;
    gui_rf_preview_worker_get_stats(worker, &last);
    check(last.processing_cpu_ms == 31.25 && last.total_frames == 4,
          "nearby blocks account both complete Windows-sized CPU quanta exactly once");
    printf("{\"case\":\"short gaps and control reset retain coarse accounting\","
           "\"processing_cpu_ms\":%.3f,\"frames\":%llu}\n",
           last.processing_cpu_ms, (unsigned long long)last.total_frames);
    gui_rf_preview_worker_destroy(worker);
}

int main(int argc, char **argv) {
    bool expect_before = argc == 2 && strcmp(argv[1], "--expect-before") == 0;
    if (argc > 2 || (argc == 2 && !expect_before)) {
        fprintf(stderr, "Usage: %s [--expect-before]\n", argv[0]);
        return 2;
    }
    atomic_init(&injected_cpu_us, 0);
    atomic_init(&injected_clock_reads, 0);
    atomic_init(&hold_process, false);
    atomic_init(&process_entered, false);
    atomic_init(&process_calls, 0);
    atomic_init(&reset_calls, 0);
    atomic_init(&create_calls, 0);
    atomic_init(&busy_ms, 0);
    atomic_init(&active_processes, 0);
    atomic_init(&max_active_processes, 0);
    atomic_init(&latest_first_sample, 0);

    check(gui_rf_preview_worker_set_budget(1.0), "probe uses shared one-core budget");
    gui_rf_preview_worker_t *worker = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(worker != NULL, "production worker created");
    if (!worker) return 1;
    check(idle_probe_wait(&injected_clock_reads, 1, 3000), "initial CPU baseline acquired");
    check(idle_probe_finish_marker(worker, 1, 1000, 17), "warm session completes one CPU-ms block");
    gui_rf_preview_stats_t before;
    gui_rf_preview_worker_get_stats(worker, &before);
    check(before.processing_cpu_ms == 1.0 && before.total_frames == 1,
          "controlled warm CPU and frame counters");

    /* Leave the same worker idle for a real quiet wall interval, then advance
     * the CPU counter to represent prior idle wakeup work. A normal reconnect
     * supplies only a new stream epoch; it need not call worker_reset. */
    thrd_sleep_ms(250);
    atomic_store(&injected_cpu_us, 501000);
    check(idle_probe_finish_marker(worker, 2, 502000, 19), "resumed first block completes");
    gui_rf_preview_stats_t resumed;
    gui_rf_preview_worker_get_stats(worker, &resumed);
    double charged_cpu_ms = resumed.processing_cpu_ms - before.processing_cpu_ms;
    uint64_t now = rf_now_us(worker);
    rf_budget_lock_blocking();
    uint64_t debt_us = rf_budget.deadline_us > now ? rf_budget.deadline_us - now : 0;
    atomic_flag_clear_explicit(&rf_budget_lock, memory_order_release);

    uint64_t wait_start = rf_now_us(worker);
    check(submit_marker(worker, 20000000u, 32, 2, false, 23), "second resumed block enqueued");
    check(gui_rf_preview_worker_wait_idle(worker, 3000), "second block settles with bounded wait");
    uint64_t settle_us = rf_now_us(worker) - wait_start;
    gui_rf_preview_stats_t after;
    gui_rf_preview_worker_get_stats(worker, &after);
    uint64_t new_drops = after.blocks_dropped - resumed.blocks_dropped;
    uint64_t new_frames = after.total_frames - resumed.total_frames;
    printf("{\"probe\":\"injected idle CPU, not real idle wall time\","
           "\"idle_cpu_ms\":500,\"first_resumed_dsp_cpu_ms\":1,"
           "\"first_resumed_charged_cpu_ms\":%.3f,\"new_budget_debt_ms\":%.3f,"
           "\"next_block_settle_ms\":%.3f,\"next_block_frames\":%llu,"
           "\"next_block_queue_drops\":%llu}\n",
           charged_cpu_ms, debt_us / 1000.0, settle_us / 1000.0,
           (unsigned long long)new_frames, (unsigned long long)new_drops);
    if (!expect_before) {
        check(charged_cpu_ms <= 2.0, "idle CPU excluded from resumed processing accounting");
        check(debt_us < 20000u, "resume creates only current-block debt");
        check(new_drops == 0 && new_frames == 1, "next resumed block decodes instead of aging out");
    } else {
        check(charged_cpu_ms == 501.0, "old code charges the complete idle CPU interval");
        check(debt_us > 300000u, "old idle interval becomes fresh processing debt");
        check(new_drops == 1 && new_frames == 0, "fresh input ages past 200ms while paying old idle CPU");
    }
    gui_rf_preview_worker_destroy(worker);
    idle_probe_short_coarse_accounting();
    check(gui_rf_preview_worker_set_budget(1.0), "probe restores default shared budget");
    if (!failures) puts(expect_before ? "Idle-accounting old-behavior evidence reproduced" : "Idle-accounting regression passed");
    return failures ? 1 : 0;
}
#else
#include <stdio.h>
int main(void) {
    puts("This timing-injection evidence probe requires Windows GetThreadTimes.");
    return 77;
}
#endif
