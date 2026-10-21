#include "gui_rf_preview_policy.h"
#include <limits.h>
#include <string.h>

static uint64_t add_sat(uint64_t a,uint64_t b) {return UINT64_MAX-a<b ? UINT64_MAX : a+b;}
static int64_t capacity(const gui_rf_preview_policy_t *p) {
    uint64_t n=(uint64_t)p->cores_millionths*4000u/1000000u;
    if(n<GUI_RF_POLICY_RESERVE_CPU_US) n=GUI_RF_POLICY_RESERVE_CPU_US;
    return (int64_t)(n+(uint64_t)p->counter_count*p->accounting_quantum_cpu_us);
}
static unsigned int concurrency(const gui_rf_preview_policy_t *p) {
    return p->cores_millionths ? (p->cores_millionths+999999u)/1000000u : GUI_RF_POLICY_MAX_ACTIVE;
}
static int64_t unsettled_capacity(const gui_rf_preview_policy_t *p,uint64_t now) {
    uint64_t limit=(uint64_t)capacity(p);
    for(unsigned int i=0;i<p->active_count;i++) {
        uint64_t elapsed=now>=p->active_start_us[i] ? now-p->active_start_us[i] : 0;
        limit=add_sat(limit,elapsed);
    }
    return limit>(uint64_t)INT64_MAX ? INT64_MAX : (int64_t)limit;
}
static void refresh(gui_rf_preview_policy_t *p,uint64_t now) {
    /* Different consumers can sample before waiting for the policy lock. A
     * late older sample must not rewind the shared refill clock. */
    if(p->initialized && now<p->updated_us) now=p->updated_us;
    if(!p->initialized) {p->credit_cpu_us=capacity(p);p->updated_us=now;p->initialized=true;}
    uint64_t elapsed=now>=p->updated_us ? now-p->updated_us : 0;
    p->updated_us=now;
    if(!p->cores_millionths) {p->credit_cpu_us=capacity(p);p->deadline_us=now;return;}
    /* Long idle saturates at capacity; bounded pieces avoid multiplication
     * overflow even for synthetic UINT64_MAX clock/cost tests. */
    uint64_t refill=elapsed/1000000u;
    refill=refill>UINT64_MAX/p->cores_millionths ? UINT64_MAX : refill*p->cores_millionths;
    refill=add_sat(refill,(elapsed%1000000u)*p->cores_millionths/1000000u);
    if(refill>(uint64_t)INT64_MAX || p->credit_cpu_us>INT64_MAX-(int64_t)refill)
        p->credit_cpu_us=unsettled_capacity(p,now);
    else {
        p->credit_cpu_us+=(int64_t)refill;
        if(p->credit_cpu_us>unsettled_capacity(p,now)) p->credit_cpu_us=unsettled_capacity(p,now);
    }
    double missing=(double)GUI_RF_POLICY_RESERVE_CPU_US-(double)p->credit_cpu_us;
    double delay=missing>0 ? missing*1000000.0/p->cores_millionths+.999999 : 0;
    uint64_t wait=delay>=(double)UINT64_MAX ? UINT64_MAX : (uint64_t)delay;
    p->deadline_us=add_sat(now,wait);
}
bool gui_rf_preview_policy_register_counter(gui_rf_preview_policy_t *p,uintptr_t owner) {
    if(!owner) return false;
    for(unsigned int i=0;i<p->counter_count;i++) if(p->counters[i]==owner) return true;
    if(p->counter_count==GUI_RF_POLICY_MAX_COUNTERS) return false;
    p->counters[p->counter_count++]=owner;
    return true;
}
void gui_rf_preview_policy_unregister_counter(gui_rf_preview_policy_t *p,uintptr_t owner) {
    for(unsigned int i=0;i<p->counter_count;i++) if(p->counters[i]==owner) {
        memmove(p->counters+i,p->counters+i+1,(p->counter_count-i-1)*sizeof(*p->counters));
        p->counter_count--;
        /* No refill or interval reset: in particular a negative balance stays
         * negative when a stopped counter removes its accounting allowance. */
        if(p->initialized && p->credit_cpu_us>unsettled_capacity(p,p->updated_us))
            p->credit_cpu_us=unsettled_capacity(p,p->updated_us);
        return;
    }
}

void gui_rf_preview_policy_retire_counter(gui_rf_preview_policy_t *p,uintptr_t owner,uint64_t measured,uint64_t residual) {
    bool registered=false;
    for(unsigned int i=0;i<p->counter_count;i++) registered|=p->counters[i]==owner;
    if(!registered) return;
    if(residual>p->accounting_quantum_cpu_us) residual=p->accounting_quantum_cpu_us;
    uint64_t charge=add_sat(measured,residual);
    if(p->initialized && p->cores_millionths && charge) {
        if(charge>(uint64_t)INT64_MAX || p->credit_cpu_us<INT64_MIN+(int64_t)charge) p->credit_cpu_us=INT64_MIN;
        else p->credit_cpu_us-=(int64_t)charge;
    }
    gui_rf_preview_policy_unregister_counter(p,owner);
}

bool gui_rf_preview_policy_set_budget(gui_rf_preview_policy_t *p,double cores) {
    uint64_t bits;memcpy(&bits,&cores,sizeof(bits));
    if(((bits>>52)&0x7ffu)==0x7ffu || cores<0 || cores>8 || (cores>0 && cores<.05)) return false;
    p->cores_millionths=(uint32_t)(cores*1000000+.5);
    p->initialized=false; /* Explicit reconfiguration starts a fresh bounded interval. */
    p->deadline_us=0;
    return true;
}
void gui_rf_preview_policy_cancel(gui_rf_preview_policy_t *p,uintptr_t owner) {
    for(unsigned int i=0;i<p->waiter_count;i++) if(p->waiters[i]==owner) {
        memmove(p->waiters+i,p->waiters+i+1,(p->waiter_count-i-1)*sizeof(*p->waiters));
        p->waiter_count--;return;
    }
}
bool gui_rf_preview_policy_acquire(gui_rf_preview_policy_t *p,uintptr_t owner,uint64_t now,uint64_t *wait) {
    if(!gui_rf_preview_policy_register_counter(p,owner)) {*wait=1000;return false;}
    if(p->initialized && now<p->updated_us) now=p->updated_us;
    refresh(p,now);
    bool queued=false;
    for(unsigned int i=0;i<p->active_count;i++) if(p->active[i]==owner) {*wait=1000;return false;}
    for(unsigned int i=0;i<p->waiter_count;i++) queued|=p->waiters[i]==owner;
    if(!owner || (!queued && p->waiter_count==64)) {*wait=1000;return false;}
    if(!queued) p->waiters[p->waiter_count++]=owner;
    if(p->active_count>=concurrency(p) || p->waiters[0]!=owner) {*wait=1000;return false;}
    if(p->cores_millionths && p->credit_cpu_us<(int64_t)GUI_RF_POLICY_RESERVE_CPU_US) {
        *wait=p->deadline_us>now ? p->deadline_us-now : 1;return false;
    }
    gui_rf_preview_policy_cancel(p,owner);
    p->active[p->active_count]=owner;p->active_start_us[p->active_count++]=now;
    if(p->cores_millionths) p->credit_cpu_us-=GUI_RF_POLICY_RESERVE_CPU_US;
    *wait=0;return true;
}
void gui_rf_preview_policy_finish(gui_rf_preview_policy_t *p,uintptr_t owner,uint64_t now,uint64_t cpu) {
    unsigned int at=0;
    while(at<p->active_count && p->active[at]!=owner) at++;
    if(at==p->active_count) return;
    if(p->initialized && now<p->updated_us) now=p->updated_us;
    refresh(p,now);
    memmove(p->active+at,p->active+at+1,(p->active_count-at-1)*sizeof(*p->active));
    memmove(p->active_start_us+at,p->active_start_us+at+1,(p->active_count-at-1)*sizeof(*p->active_start_us));p->active_count--;
    if(p->cores_millionths) {
        if(cpu>GUI_RF_POLICY_RESERVE_CPU_US) {
            uint64_t excess=cpu-GUI_RF_POLICY_RESERVE_CPU_US;
            if(excess>(uint64_t)INT64_MAX || p->credit_cpu_us<INT64_MIN+(int64_t)excess)
                p->credit_cpu_us=INT64_MIN;
            else p->credit_cpu_us-=(int64_t)excess;
        } else {
            int64_t refund=GUI_RF_POLICY_RESERVE_CPU_US-(int64_t)cpu;
            if(p->credit_cpu_us>INT64_MAX-refund) p->credit_cpu_us=INT64_MAX;
            else p->credit_cpu_us+=refund;
        }
    }
    refresh(p,now);
}
