/* Deterministic Windows FIFO notification regression. There are no running
 * workers and no wall-clock deadlines: real auto-reset event state proves the
 * handoff after a successor consumed its old wake before the head acquired.
 * Include the production worker privately, reusing only existing DSP stubs. */
#ifdef _WIN32
#define main gui_rf_preview_existing_worker_test_main
#include "gui_rf_preview_worker_test.c"
#undef main
#include "../misrc_gui/processing/gui_rf_preview_worker.c"

static bool handoff_init(gui_rf_preview_worker_t *w) {
    memset(w, 0, sizeof(*w));
    atomic_init(&w->stop, false);
    atomic_init(&w->flush_requested, false);
    atomic_init(&w->control, 0);
    atomic_init(&w->pause_control, 0);
    atomic_init(&w->budget_waiting, false);
    atomic_init(&w->audio_enabled, true);
    atomic_init(&w->audio_generation, 0);
    atomic_init(&w->audio_budget_waiting, false);
    atomic_init(&w->requested_profile, GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    LARGE_INTEGER frequency;
    if (QueryPerformanceFrequency(&frequency)) w->timer_frequency = (uint64_t)frequency.QuadPart;
    w->input_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    w->audio_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    return rf_mutex_init(&w->frame_mutex) && w->input_event && w->audio_event;
}

static HANDLE handoff_event(gui_rf_preview_worker_t *w, bool audio) {
    return audio ? w->audio_event : w->input_event;
}

static bool handoff_acquire(gui_rf_preview_worker_t *w, bool audio) {
    return audio ? rf_audio_acquire_budget(w, 0) : rf_acquire_budget(w, 0, 0);
}

static void test_eligible_head(double cores, bool first_audio, bool next_audio) {
    gui_rf_preview_worker_t *workers = calloc(3, sizeof(*workers));
    check(workers != NULL, "handoff owners allocated");
    if (!workers) return;
    bool initialized = true;
    for (unsigned int i = 0; i < 3; i++) initialized &= handoff_init(&workers[i]);
    check(initialized, "handoff auto-reset events and mutexes created");
    if (initialized) {
        rf_budget = (gui_rf_preview_policy_t)GUI_RF_PREVIEW_POLICY_INITIALIZER;
        check(gui_rf_preview_policy_set_budget(&rf_budget, cores), "handoff policy budget accepted");
        uintptr_t first = (uintptr_t)&workers[0] | (first_audio ? 1u : 0u);
        uintptr_t next = (uintptr_t)&workers[1] | (next_audio ? 1u : 0u);
        uintptr_t last = (uintptr_t)&workers[2];
        rf_budget.waiters[0] = first;
        rf_budget.waiters[1] = next;
        rf_budget.waiters[2] = last;
        rf_budget.waiter_count = 3;
        /* The successor woke too early while the previous owner was still
         * FIFO head. Its old auto-reset signal is now gone, with no producer
         * input or finish notification available to supply another one. */
        check(SetEvent(handoff_event(&workers[1], next_audio)) &&
              WaitForSingleObject(handoff_event(&workers[1], next_audio), 0) == WAIT_OBJECT_0 &&
              WaitForSingleObject(handoff_event(&workers[1], next_audio), 0) == WAIT_TIMEOUT,
              "successor consumed its earlier notification before head acquisition");
        check(handoff_acquire(&workers[0], first_audio), "old FIFO head acquires through production entry");
        check(rf_budget.active_count == 1 && rf_budget.waiter_count == 2 && rf_budget.waiters[0] == next,
              "head removal leaves an eligible successor and one free permit");
        check(WaitForSingleObject(handoff_event(&workers[1], next_audio), 0) == WAIT_OBJECT_0,
              "successful video/audio acquire re-notifies the eligible new FIFO head");
        check(WaitForSingleObject(handoff_event(&workers[1], !next_audio), 0) == WAIT_TIMEOUT &&
              WaitForSingleObject(workers[2].input_event, 0) == WAIT_TIMEOUT &&
              WaitForSingleObject(workers[2].audio_event, 0) == WAIT_TIMEOUT,
              "head handoff targets only its selected video/audio event");
        check(handoff_acquire(&workers[1], next_audio), "new FIFO head acquires second permit without a finish");
        check(rf_budget.active_count == 2 && rf_budget.active[0] == first && rf_budget.active[1] == next &&
              rf_budget.waiter_count == 1 && rf_budget.waiters[0] == last,
              "both consumers retain FIFO order and independent permits");
        check(WaitForSingleObject(workers[2].input_event, 0) == WAIT_TIMEOUT,
              "full permit set does not wake the third consumer");
    }
    rf_budget = (gui_rf_preview_policy_t)GUI_RF_PREVIEW_POLICY_INITIALIZER;
    for (unsigned int i = 0; i < 3; i++) {
        if (workers[i].input_event) CloseHandle(workers[i].input_event);
        if (workers[i].audio_event) CloseHandle(workers[i].audio_event);
        rf_mutex_destroy(&workers[i].frame_mutex);
    }
    free(workers);
}

static void test_no_eligible_head(bool exhausted_credit) {
    gui_rf_preview_worker_t workers[2];
    bool initialized = handoff_init(&workers[0]);
    initialized &= handoff_init(&workers[1]);
    check(initialized, "ineligible handoff fixtures initialized");
    if (initialized) {
        rf_budget = (gui_rf_preview_policy_t)GUI_RF_PREVIEW_POLICY_INITIALIZER;
        check(gui_rf_preview_policy_set_budget(&rf_budget, exhausted_credit ? 2 : 1),
              "ineligible handoff budget accepted");
        rf_budget.waiters[0] = (uintptr_t)&workers[0];
        rf_budget.waiters[1] = (uintptr_t)&workers[1];
        rf_budget.waiter_count = 2;
        if (exhausted_credit) {
            rf_budget.initialized = true;
            /* Prevent elapsed refill without replacing the production clock. */
            rf_budget.updated_us = UINT64_MAX;
            rf_budget.credit_cpu_us = GUI_RF_POLICY_RESERVE_CPU_US;
        }
        check(rf_acquire_budget(&workers[0], 0, 0), "ineligible fixture head still acquires first permit");
        check(rf_budget.active_count == 1 && rf_budget.waiter_count == 1 &&
              WaitForSingleObject(workers[1].input_event, 0) == WAIT_TIMEOUT,
              "no redundant wake when permits or reservation credit are exhausted");
    }
    rf_budget = (gui_rf_preview_policy_t)GUI_RF_PREVIEW_POLICY_INITIALIZER;
    for (unsigned int i = 0; i < 2; i++) {
        if (workers[i].input_event) CloseHandle(workers[i].input_event);
        if (workers[i].audio_event) CloseHandle(workers[i].audio_event);
        rf_mutex_destroy(&workers[i].frame_mutex);
    }
}

int main(void) {
    const double budgets[] = {1.5, 2};
    for (unsigned int b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++)
        for (unsigned int first_audio = 0; first_audio < 2; first_audio++)
            for (unsigned int next_audio = 0; next_audio < 2; next_audio++)
                test_eligible_head(budgets[b], first_audio != 0, next_audio != 0);
    test_no_eligible_head(false);
    test_no_eligible_head(true);
    if (!failures) puts("RF preview FIFO head notifications passed (8 interleavings, 2 ineligible cases)");
    return failures ? 1 : 0;
}
#else
int main(void) { return 0; }
#endif
