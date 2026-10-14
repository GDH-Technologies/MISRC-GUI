#ifndef GUI_RF_PREVIEW_POLICY_H
#define GUI_RF_PREVIEW_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Pure deterministic policy. Caller serializes access. At most one processing
 * block holds the permit, so sharing never multiplies the configured budget.
 * Clock arguments are monotonic wall microseconds; cost is thread CPU use or
 * conservative work wall time if thread CPU accounting is unavailable. */
typedef struct {
    uint32_t cores_millionths;
    uint64_t deadline_us;
    uint64_t block_start_us;
    bool busy;
    uintptr_t waiters[64];
    unsigned int waiter_count;
    /* CPU credit kept solely for coarse OS accounting; default zero. */
    uint32_t accounting_slack_cpu_us;
} gui_rf_preview_policy_t;

#define GUI_RF_PREVIEW_POLICY_INITIALIZER {1000000u, 0, 0, false, {0}, 0, 0}
bool gui_rf_preview_policy_set_budget(gui_rf_preview_policy_t *policy, double cores);
/* Nonzero owner identifies a worker. FIFO arbitration covers up to 64 waiting
 * workers (GUI pool has four). Cancel any unsuccessful request on stop/control
 * interruption; owner is never dereferenced. */
bool gui_rf_preview_policy_acquire(gui_rf_preview_policy_t *policy, uintptr_t owner,
                                   uint64_t now_us,
                                   uint64_t *wait_us);
void gui_rf_preview_policy_cancel(gui_rf_preview_policy_t *policy, uintptr_t owner);
void gui_rf_preview_policy_finish(gui_rf_preview_policy_t *policy, uint64_t cpu_us);

#endif
