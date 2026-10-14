#include "gui_rf_preview_policy.h"

#include <string.h>

bool gui_rf_preview_policy_set_budget(gui_rf_preview_policy_t *p, double cores) {
    /* Bit classification stays valid under the project's -ffast-math build. */
    uint64_t bits;
    _Static_assert(sizeof(bits) == sizeof(cores), "IEEE-754 double required");
    memcpy(&bits, &cores, sizeof(bits));
    if (((bits >> 52) & 0x7ffu) == 0x7ffu || cores < 0.0 || cores > 1.0 ||
        (cores > 0.0 && cores < 0.05)) return false;
    p->cores_millionths = (uint32_t)(cores * 1000000.0 + 0.5);
    /* Configuration starts a fresh pacing interval; an active block is still
     * accounted at finish using the new rate. Only the explicitly configured
     * OS accounting slack can carry credit; it cannot accumulate unbounded. */
    p->deadline_us = 0;
    return true;
}

void gui_rf_preview_policy_cancel(gui_rf_preview_policy_t *p, uintptr_t owner) {
    for (unsigned int i = 0; i < p->waiter_count; i++) {
        if (p->waiters[i] != owner) continue;
        memmove(p->waiters + i, p->waiters + i + 1,
                (p->waiter_count - i - 1) * sizeof(*p->waiters));
        p->waiter_count--;
        return;
    }
}

bool gui_rf_preview_policy_acquire(gui_rf_preview_policy_t *p, uintptr_t owner, uint64_t now_us,
                                   uint64_t *wait_us) {
    bool queued = false;
    for (unsigned int i = 0; i < p->waiter_count; i++) queued |= p->waiters[i] == owner;
    if (!owner || (!queued && p->waiter_count == 64)) { *wait_us = 1000; return false; }
    if (!queued) p->waiters[p->waiter_count++] = owner;
    if (p->busy) { *wait_us = 1000; return false; }
    if (p->cores_millionths && p->deadline_us > now_us) {
        *wait_us = p->deadline_us - now_us;
        return false;
    }
    if (p->waiters[0] != owner) { *wait_us = 1000; return false; }
    gui_rf_preview_policy_cancel(p, owner);
    p->busy = true;
    p->block_start_us = now_us;
    if (p->deadline_us && p->cores_millionths && p->accounting_slack_cpu_us) {
        uint64_t slack_wall = ((uint64_t)p->accounting_slack_cpu_us * 1000000u +
                                p->cores_millionths - 1) / p->cores_millionths;
        uint64_t earliest = now_us > slack_wall ? now_us - slack_wall : 0;
        p->block_start_us = p->deadline_us > earliest ? p->deadline_us : earliest;
    }
    *wait_us = 0;
    return true;
}

void gui_rf_preview_policy_finish(gui_rf_preview_policy_t *p, uint64_t cpu_us) {
    if (!p->busy) return;
    uint64_t duration = 0;
    if (p->cores_millionths) {
        uint64_t quotient = cpu_us / p->cores_millionths;
        uint64_t remainder = cpu_us % p->cores_millionths;
        if (quotient > UINT64_MAX / 1000000u) duration = UINT64_MAX;
        else {
            duration = quotient * 1000000u;
            uint64_t extra = (remainder * 1000000u + p->cores_millionths - 1) /
                             p->cores_millionths;
            duration = UINT64_MAX - duration < extra ? UINT64_MAX : duration + extra;
        }
    }
    p->deadline_us = UINT64_MAX - p->block_start_us < duration ? UINT64_MAX :
                     p->block_start_us + duration;
    p->busy = false;
}
