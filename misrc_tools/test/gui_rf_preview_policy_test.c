#include "../misrc_gui/processing/gui_rf_preview_policy.h"
#include <stdio.h>
#include <string.h>
static int failures;
static void check(bool okay,const char *label) {if(!okay) {fprintf(stderr,"FAIL: %s\n",label);failures++;}}
static void test_coarse_elapsed_credit(double cores,unsigned int counters) {
    gui_rf_preview_policy_t p=GUI_RF_PREVIEW_POLICY_INITIALIZER;
    p.accounting_quantum_cpu_us=15625;
    for(uintptr_t owner=1;owner<=counters;owner++)
        check(gui_rf_preview_policy_register_counter(&p,owner),"coarse CPU counter registration");
    check(gui_rf_preview_policy_set_budget(&p,cores),"coarse elapsed-credit budget");
    uint64_t now=0,wait=0,total_cpu=0,extra_wait=0;
    const uint64_t quantum=15625;
    uint64_t period=(uint64_t)((double)(counters*quantum)/cores+.999999);
    for(unsigned int cycle=0;cycle<100;cycle++) {
        uint64_t end=now+period;
        /* Real CPU is already being spent; Windows exposes zero snapshots
         * across those earlier wall intervals, then posts each whole quantum. */
        for(uint64_t at=now;at+1000<end;at+=1000) {
            uintptr_t owner=1+(at/1000)%counters;
            uint64_t start=at;
            while(!gui_rf_preview_policy_acquire(&p,owner,start,&wait)) {start+=wait;extra_wait+=wait;}
            gui_rf_preview_policy_finish(&p,owner,start,0);
        }
        now=end;
        for(uintptr_t owner=1;owner<=counters;owner++) {
            while(!gui_rf_preview_policy_acquire(&p,owner,now,&wait)) {now+=wait;extra_wait+=wait;}
            gui_rf_preview_policy_finish(&p,owner,now,quantum);total_cpu+=quantum;
        }
    }
    double actual=(double)total_cpu/now;
    check(extra_wait==0,"late coarse CPU quantum uses already elapsed retained credit without artificial sleep");
    check(actual>=cores*.995 && actual<=cores*1.005,"interleaved coarse counters sustain the configured aggregate utilization");
    uint64_t long_idle=now+UINT64_C(1000000000);
    check(gui_rf_preview_policy_acquire(&p,1,long_idle,&wait),"coarse counter resumes after long idle");
    gui_rf_preview_policy_finish(&p,1,long_idle,0);
    int64_t ceiling=(int64_t)(cores*4000)+(int64_t)counters*15625;
    if(ceiling<(int64_t)(1000+counters*15625)) ceiling=1000+counters*15625;
    check(p.credit_cpu_us==ceiling,"long idle credit stays bounded by explicit counter quanta and base capacity");
    gui_rf_preview_policy_unregister_counter(&p,1);
    check(p.counter_count==counters-1 && p.credit_cpu_us<=ceiling-15625,"retired counter removes positive accounting allowance");
}
static void test_counter_registry_debt(void) {
    gui_rf_preview_policy_t p=GUI_RF_PREVIEW_POLICY_INITIALIZER;p.accounting_quantum_cpu_us=15625;
    uint64_t wait=0;check(gui_rf_preview_policy_acquire(&p,1,0,&wait),"counter debt fixture grant");
    gui_rf_preview_policy_finish(&p,1,0,50000);
    int64_t debt=p.credit_cpu_us;check(debt<0,"counter fixture has real debt");
    gui_rf_preview_policy_unregister_counter(&p,1);
    check(p.credit_cpu_us==debt && p.counter_count==0,"counter retirement never erases debt");
    check(gui_rf_preview_policy_register_counter(&p,2) && p.credit_cpu_us==debt,"new counter cannot reset inherited debt");
    check(!gui_rf_preview_policy_acquire(&p,2,0,&wait),"replacement counter still pays global debt");
    gui_rf_preview_policy_cancel(&p,2);gui_rf_preview_policy_unregister_counter(&p,2);
    for(uintptr_t owner=1;owner<=GUI_RF_POLICY_MAX_COUNTERS;owner++)
        check(gui_rf_preview_policy_register_counter(&p,owner),"bounded registry slot admitted");
    check(!gui_rf_preview_policy_register_counter(&p,GUI_RF_POLICY_MAX_COUNTERS+1),"registry limit forbids unlimited accounting credit");
    check(gui_rf_preview_policy_register_counter(&p,1) && p.counter_count==GUI_RF_POLICY_MAX_COUNTERS,"duplicate counter cannot multiply allowance");
}
static void test_short_counter_retirement(void) {
    gui_rf_preview_policy_t p=GUI_RF_PREVIEW_POLICY_INITIALIZER;p.accounting_quantum_cpu_us=15625;
    check(gui_rf_preview_policy_set_budget(&p,.5),"short counter retirement budget");
    uint64_t now=0,wait=0,waited=0;
    for(uintptr_t owner=1;owner<=100;owner++) {
        check(gui_rf_preview_policy_register_counter(&p,owner),"short-lived counter registers");
        while(!gui_rf_preview_policy_acquire(&p,owner,now,&wait)) {now+=wait;waited+=wait;}
        now+=1000;gui_rf_preview_policy_finish(&p,owner,now,0);
        gui_rf_preview_policy_retire_counter(&p,owner,0,1000);
        check(p.counter_count==0,"short-lived counter retires once");
        int64_t balance=p.credit_cpu_us;
        gui_rf_preview_policy_retire_counter(&p,owner,0,1000);
        check(p.credit_cpu_us==balance,"duplicate retirement cannot debit twice");
    }
    check(gui_rf_preview_policy_set_budget(&p,1),"final measured/residual reconciliation budget");
    check(gui_rf_preview_policy_acquire(&p,101,now,&wait),"final reconciliation owner grant");
    gui_rf_preview_policy_finish(&p,101,now,0);
    int64_t before=p.credit_cpu_us;
    gui_rf_preview_policy_retire_counter(&p,101,15625,1000);
    check(p.credit_cpu_us==before-16625 && p.counter_count==0,"newly visible final CPU is exact and precedes bounded residual retirement");
    double ratio=100000.0/now;
    check(waited>90000 && ratio>.49 && ratio<.52,"repeated unbooked short workers cannot bypass half-core aggregate budget");
}

int main(void) {
    gui_rf_preview_policy_t p=GUI_RF_PREVIEW_POLICY_INITIALIZER;uint64_t wait=0;
    check(gui_rf_preview_policy_acquire(&p,1,0,&wait),"initial bounded token grant");
    check(!gui_rf_preview_policy_acquire(&p,2,0,&wait),"one-core budget remains serialized");
    gui_rf_preview_policy_finish(&p,1,1000,8000);
    check(!gui_rf_preview_policy_acquire(&p,2,1000,&wait) && wait==4000,"CPU overrun creates exact aggregate debt");
    check(!gui_rf_preview_policy_acquire(&p,1,5000,&wait),"FIFO prevents previous owner stealing");
    check(gui_rf_preview_policy_acquire(&p,2,5000,&wait),"FIFO head resumes at exact debt deadline");
    gui_rf_preview_policy_finish(&p,2,5000,1000);
    gui_rf_preview_policy_cancel(&p,1);
    check(gui_rf_preview_policy_set_budget(&p,2),"two-core budget accepted");
    check(gui_rf_preview_policy_acquire(&p,1,10000,&wait) && gui_rf_preview_policy_acquire(&p,2,10000,&wait),"two independent concurrent reservations");
    check(!gui_rf_preview_policy_acquire(&p,3,10000,&wait),"ceil budget bounds active owners");
    gui_rf_preview_policy_finish(&p,2,11000,3000);
    check(!gui_rf_preview_policy_acquire(&p,2,11000,&wait),"finished owner queues behind existing contender");
    check(gui_rf_preview_policy_acquire(&p,3,11000,&wait),"out-of-order finish releases its own permit");
    check(p.active_count==2 && p.credit_cpu_us==5000,"parallel reservations retain one shared token account");
    gui_rf_preview_policy_finish(&p,1,12000,2000);gui_rf_preview_policy_finish(&p,3,12000,2000);
    gui_rf_preview_policy_cancel(&p,2);
    check(gui_rf_preview_policy_set_budget(&p,1.5),"fractional multi-core budget");
    check(gui_rf_preview_policy_acquire(&p,1,20000,&wait) && gui_rf_preview_policy_acquire(&p,2,20000,&wait),"fractional budget admits second consumer");
    check(!gui_rf_preview_policy_acquire(&p,3,20000,&wait),"fractional concurrency rounds up with finite bound");
    gui_rf_preview_policy_finish(&p,1,20000,1000);gui_rf_preview_policy_finish(&p,2,20000,1000);gui_rf_preview_policy_cancel(&p,3);
    check(gui_rf_preview_policy_set_budget(&p,.5),"half-core pacing");
    check(gui_rf_preview_policy_acquire(&p,1,30000,&wait),"half-core starts finite burst");
    gui_rf_preview_policy_finish(&p,1,30000,5000);
    check(!gui_rf_preview_policy_acquire(&p,1,30000,&wait) && wait==8000,"half-core debt includes reservation without multiplication");
    gui_rf_preview_policy_cancel(&p,1);
    check(gui_rf_preview_policy_acquire(&p,1,100000,&wait),"long work interval starts");
    check(!gui_rf_preview_policy_acquire(&p,2,120000,&wait),"contender cannot steal long active owner");
    gui_rf_preview_policy_finish(&p,1,140000,20000);
    check(p.credit_cpu_us==2000,"elapsed budget credit offsets active cost before idle clamp");
    gui_rf_preview_policy_cancel(&p,2);
    check(gui_rf_preview_policy_set_budget(&p,.5),"long-block utilization budget");
    uint64_t clock=1000000,used=0;
    for(unsigned int block=0;block<100;block++) {
        while(!gui_rf_preview_policy_acquire(&p,1,clock,&wait)) clock+=wait;
        for(unsigned int poll=0;poll<20;poll++) {
            clock+=1000;check(!gui_rf_preview_policy_acquire(&p,2,clock,&wait),"inflight long work retains one permit");
        }
        used+=20000;gui_rf_preview_policy_finish(&p,1,clock,20000);gui_rf_preview_policy_cancel(&p,2);
    }
    double ratio=(double)used/(clock-1000000);
    check(ratio>=.49 && ratio<=.52,"many 20ms CPU blocks approach half-core budget without artificial idling");
    check(gui_rf_preview_policy_set_budget(&p,1),"stale policy-clock fixture");
    check(gui_rf_preview_policy_acquire(&p,1,1000,&wait),"stale-clock active owner");
    check(!gui_rf_preview_policy_acquire(&p,2,10000,&wait),"contender advances refill clock");
    gui_rf_preview_policy_finish(&p,1,9000,30000);
    check(p.updated_us==10000 && p.credit_cpu_us==-17000,"late older finish neither rewinds clock nor earns duplicate CPU credit");
    check(!gui_rf_preview_policy_acquire(&p,2,10000,&wait) && p.credit_cpu_us==-17000,"same real clock cannot refill a stale interval twice");
    gui_rf_preview_policy_cancel(&p,2);
    check(gui_rf_preview_policy_set_budget(&p,2),"coarse-counter fixture budget");
    for(int i=0;i<20;i++) {
        check(gui_rf_preview_policy_acquire(&p,1,100000,&wait),"valid zero CPU completion may return reservation");
        gui_rf_preview_policy_finish(&p,1,100000,0);
    }
    check(gui_rf_preview_policy_acquire(&p,1,100000,&wait),"coarse quantum permit");
    gui_rf_preview_policy_finish(&p,1,100000,15625);
    check(p.credit_cpu_us==-7625,"zero blocks plus one OS quantum charge exactly once");
    check(!gui_rf_preview_policy_acquire(&p,1,100000,&wait) && wait==4313,"coarse quantum creates bounded shared debt");
    gui_rf_preview_policy_cancel(&p,1);
    check(gui_rf_preview_policy_acquire(&p,1,1000000,&wait),"long idle refills only bounded capacity");
    check(p.credit_cpu_us==7000,"idle cannot accumulate more than four ms CPU budget");
    gui_rf_preview_policy_finish(&p,1,1000000,0);
    uint64_t nan_bits=UINT64_C(0x7ff8000000000001);double nan;memcpy(&nan,&nan_bits,sizeof(nan));
    check(!gui_rf_preview_policy_set_budget(&p,nan) && !gui_rf_preview_policy_set_budget(&p,-.5) &&
          !gui_rf_preview_policy_set_budget(&p,.01) && !gui_rf_preview_policy_set_budget(&p,8.01),"invalid and nonfinite budgets rejected");
    check(gui_rf_preview_policy_set_budget(&p,0),"offline bypass accepted");
    for(uintptr_t i=1;i<=8;i++) check(gui_rf_preview_policy_acquire(&p,i,2000000,&wait),"offline still has bounded concurrency");
    check(!gui_rf_preview_policy_acquire(&p,9,2000000,&wait),"offline ninth active owner rejected");
    for(uintptr_t i=1;i<=8;i++) gui_rf_preview_policy_finish(&p,i,2000000,UINT64_MAX);
    check(gui_rf_preview_policy_acquire(&p,9,2000000,&wait),"offline CPU cost does not pace");gui_rf_preview_policy_finish(&p,9,2000000,0);
    check(gui_rf_preview_policy_set_budget(&p,.05),"minimum budget accepted");
    check(gui_rf_preview_policy_acquire(&p,1,UINT64_MAX-1,&wait),"near limit acquisition");
    gui_rf_preview_policy_finish(&p,1,UINT64_MAX-1,UINT64_MAX);
    check(!gui_rf_preview_policy_acquire(&p,1,UINT64_MAX-1,&wait) && wait==1,"overflow saturates debt and clock safely");
    test_coarse_elapsed_credit(1,1);
    test_coarse_elapsed_credit(1,2);
    test_coarse_elapsed_credit(.5,2);
    test_coarse_elapsed_credit(1.5,2);
    test_counter_registry_debt();
    test_short_counter_retirement();
    if(!failures) puts("RF preview aggregate parallel policy tests passed");
    return failures ? 1 : 0;
}
