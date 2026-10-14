#include "../misrc_gui/processing/gui_rf_preview_policy.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void check(bool okay, const char *label) {
    if (!okay) { fprintf(stderr, "FAIL: %s\n", label); failures++; }
}

int main(void) {
    gui_rf_preview_policy_t p = GUI_RF_PREVIEW_POLICY_INITIALIZER;
    uint64_t wait = 0;
    check(gui_rf_preview_policy_acquire(&p, 1, 1000, &wait), "first worker permit");
    check(!gui_rf_preview_policy_acquire(&p, 2, 1000, &wait) && wait == 1000,
          "second worker cannot double the global active processing allowance");
    gui_rf_preview_policy_finish(&p, 8000);
    check(!gui_rf_preview_policy_acquire(&p, 2, 8000, &wait) && wait == 1000,
          "shared one-core CPU debt remains until its wall deadline");
    check(gui_rf_preview_policy_acquire(&p, 2, 9000, &wait), "next worker at exact deadline");
    gui_rf_preview_policy_finish(&p, 8000);
    check(!gui_rf_preview_policy_acquire(&p, 1, 9000, &wait) && wait == 8000,
          "worker switch cannot reset or multiply the budget");

    check(gui_rf_preview_policy_set_budget(&p, .5), "half-core budget");
    check(gui_rf_preview_policy_acquire(&p, 1, 20000, &wait), "fresh configured interval");
    gui_rf_preview_policy_finish(&p, 5000);
    check(!gui_rf_preview_policy_acquire(&p, 1, 25000, &wait) && wait == 5000,
          "half-core processing waits for the other half of the interval");
    check(gui_rf_preview_policy_acquire(&p, 1, 1000000, &wait), "idle interval gains no credit");
    gui_rf_preview_policy_finish(&p, 1);
    check(!gui_rf_preview_policy_acquire(&p, 1, 1000000, &wait) && wait == 2,
          "microsecond fractional cost rounds conservatively");
    check(gui_rf_preview_policy_set_budget(&p, .75), "three-quarter-core budget");
    check(gui_rf_preview_policy_acquire(&p, 1, 2000000, &wait), "three-quarter-core permit");
    gui_rf_preview_policy_finish(&p, 3000);
    check(!gui_rf_preview_policy_acquire(&p, 1, 2003000, &wait) && wait == 1000,
          "three-quarter-core exact pacing");

    uint64_t nan_bits = UINT64_C(0x7ff8000000000001);
    double nan_value;
    memcpy(&nan_value, &nan_bits, sizeof(nan_value));
    check(!gui_rf_preview_policy_set_budget(&p, nan_value), "NaN rejected under fast-math");
    /* Ceiling is 8 logical cores (measured DSP need ~1.1 cores/worker):
     * multi-core budgets are accepted; only the true range edges reject. */
    check(!gui_rf_preview_policy_set_budget(&p, -.5) &&
          !gui_rf_preview_policy_set_budget(&p, .01) &&
          !gui_rf_preview_policy_set_budget(&p, 8.01) &&
          gui_rf_preview_policy_set_budget(&p, 8) &&
          gui_rf_preview_policy_set_budget(&p, 1.01), "invalid budget range rejected, multi-core budgets accepted");
    check(gui_rf_preview_policy_set_budget(&p, 0), "offline pacing bypass");
    check(gui_rf_preview_policy_acquire(&p, 1, 3000000, &wait), "offline permit");
    gui_rf_preview_policy_finish(&p, 1000000);
    check(gui_rf_preview_policy_acquire(&p, 1, 3000001, &wait), "offline has no pacing debt");
    gui_rf_preview_policy_finish(&p, 0);
    check(gui_rf_preview_policy_set_budget(&p, .05), "minimum supported budget");
    check(gui_rf_preview_policy_acquire(&p, 1, UINT64_MAX - 1, &wait), "near-clock-limit permit");
    gui_rf_preview_policy_finish(&p, UINT64_MAX);
    check(!gui_rf_preview_policy_acquire(&p, 1, UINT64_MAX - 1, &wait) && wait == 1,
          "debt arithmetic saturates rather than wrapping into immediate credit");
    gui_rf_preview_policy_t fair = GUI_RF_PREVIEW_POLICY_INITIALIZER;
    check(gui_rf_preview_policy_acquire(&fair, 1, 0, &wait), "FIFO first active worker");
    check(!gui_rf_preview_policy_acquire(&fair, 2, 0, &wait) &&
          !gui_rf_preview_policy_acquire(&fair, 3, 0, &wait), "FIFO two contenders queued");
    gui_rf_preview_policy_finish(&fair, 1000);
    check(!gui_rf_preview_policy_acquire(&fair, 1, 1000, &wait),
          "busy stream cannot immediately steal permit from older waiter");
    check(gui_rf_preview_policy_acquire(&fair, 2, 1000, &wait), "oldest waiting worker gets permit");
    gui_rf_preview_policy_finish(&fair, 1);
    gui_rf_preview_policy_cancel(&fair, 3);
    check(gui_rf_preview_policy_acquire(&fair, 1, 1001, &wait),
          "cancelled paused/stopped head does not block other workers");
    gui_rf_preview_policy_finish(&fair, 0);
    gui_rf_preview_policy_t coarse = GUI_RF_PREVIEW_POLICY_INITIALIZER;
    coarse.accounting_slack_cpu_us = 15625;
    check(gui_rf_preview_policy_acquire(&coarse, 1, 1000000, &wait), "coarse-accounting first block");
    gui_rf_preview_policy_finish(&coarse, 0);
    check(gui_rf_preview_policy_acquire(&coarse, 1, 1005000, &wait), "unbooked CPU second block");
    gui_rf_preview_policy_finish(&coarse, 0);
    check(gui_rf_preview_policy_acquire(&coarse, 1, 1020000, &wait), "coarse-accounting delayed CPU book");
    gui_rf_preview_policy_finish(&coarse, 15625);
    check(gui_rf_preview_policy_acquire(&coarse, 1, 1020000, &wait),
          "already elapsed coarse CPU book does not create duplicate pacing debt");
    gui_rf_preview_policy_finish(&coarse, 0);
    check(gui_rf_preview_policy_acquire(&coarse, 1, 5000000, &wait), "long-idle coarse-accounting block");
    gui_rf_preview_policy_finish(&coarse, 20000);
    check(!gui_rf_preview_policy_acquire(&coarse, 1, 5000000, &wait) && wait == 4375,
          "long idle cannot accumulate more than one accounting quantum of credit");
    if (!failures) puts("RF preview policy tests passed");
    return failures ? 1 : 0;
}
