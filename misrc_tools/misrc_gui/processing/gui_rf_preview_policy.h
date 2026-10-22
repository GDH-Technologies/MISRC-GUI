#ifndef GUI_RF_PREVIEW_POLICY_H
#define GUI_RF_PREVIEW_POLICY_H
#include <stdbool.h>
#include <stdint.h>

#define GUI_RF_POLICY_MAX_ACTIVE 8u
#define GUI_RF_POLICY_RESERVE_CPU_US 1000u
#define GUI_RF_POLICY_MAX_COUNTERS 64u
/* Serialized deterministic aggregate token budget. Every active owner reserves
 * CPU before work; finish reconciles its cost. Base capacity is four ms of
 * configured CPU (at least one reservation). Coarse OS accounting may retain
 * one configured quantum per registered counter, globally bounded by 64 IDs.
 * Idle credit never exceeds that explicit base plus accounting allowance.
 * ceil(cores), capped at eight, bounds concurrent noninterruptible blocks.
 * A soft budget can exceed its rate by this capacity plus active block costs;
 * an individual overlong block is charged as debt, never silently forgiven.
 * Accrued credit may also cover active owners' elapsed wall, a bound on their
 * unsettled CPU; this avoids discarding earned credit before finish debits it.
 * Once those owners finish, excess credit is clamped back to finite capacity. */
typedef struct {
    uint32_t cores_millionths;
    uint32_t accounting_quantum_cpu_us;
    uintptr_t counters[GUI_RF_POLICY_MAX_COUNTERS];
    unsigned int counter_count;
    uint64_t updated_us, deadline_us;
    int64_t credit_cpu_us;
    bool initialized;
    uintptr_t active[GUI_RF_POLICY_MAX_ACTIVE];
    uint64_t active_start_us[GUI_RF_POLICY_MAX_ACTIVE];
    unsigned int active_count;
    uintptr_t waiters[64];
    unsigned int waiter_count;
} gui_rf_preview_policy_t;
#define GUI_RF_PREVIEW_POLICY_INITIALIZER {.cores_millionths=1000000u}
bool gui_rf_preview_policy_set_budget(gui_rf_preview_policy_t *policy,double cores);
/* One ID per actual CPU counter/thread. Registration changes only the positive
 * credit ceiling, never resets time or erases existing debt. Unregister only
 * after that owner's active work and queued request have settled. */
bool gui_rf_preview_policy_register_counter(gui_rf_preview_policy_t *policy,uintptr_t owner);
void gui_rf_preview_policy_unregister_counter(gui_rf_preview_policy_t *policy,uintptr_t owner);
/* Retiring a coarse counter can leave <one quantum forever unreported. Pay
 * the caller's conservative residual (capped to that quantum), then remove
 * its allowance. This estimate is budget accounting, not measured CPU stats. */
void gui_rf_preview_policy_retire_counter(gui_rf_preview_policy_t *policy,uintptr_t owner,
                                          uint64_t measured_cpu_us,uint64_t residual_cpu_us);
bool gui_rf_preview_policy_acquire(gui_rf_preview_policy_t *policy,uintptr_t owner,
                                   uint64_t now_us,uint64_t *wait_us);
void gui_rf_preview_policy_cancel(gui_rf_preview_policy_t *policy,uintptr_t owner);
void gui_rf_preview_policy_finish(gui_rf_preview_policy_t *policy,uintptr_t owner,
                                  uint64_t now_us,uint64_t cpu_us);
#endif
