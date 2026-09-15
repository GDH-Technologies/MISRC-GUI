/*
 * Priority clamp harness (Linux only; compiled and run by ci_guard_tests.py).
 *
 * threading.h asks for SCHED_FIFO at the policy maximum and a nice of -15. A
 * process whose RLIMIT_RTPRIO / RLIMIT_NICE allow less must get the most they
 * allow, not nothing: on hosts capped at LimitRTPRIO=20 / LimitNICE=-11 the
 * all-or-nothing request failed with EPERM, then EACCES, and every "critical"
 * capture thread ran as an ordinary SCHED_OTHER thread.
 *
 * Each case runs in a forked child so scheduling state never leaks between
 * cases. Soft limits are lowered in-process (no privilege needed), so every
 * expected value below is a literal chosen by this file, never read back from
 * the code under test. Root ignores both limits, so the clamp cases skip there.
 */
#include "threading.h"

#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

#define CASE_PASS 0
#define CASE_FAIL 1
#define CASE_SKIP 2

/* ioprio_get(IOPRIO_WHO_PROCESS, id) straight from the kernel. */
static int ioprio_of(pid_t id)
{
    return (int)syscall(SYS_ioprio_get, 1, (int)id);
}

static pid_t self_tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

/* Lower (or re-raise up to the hard limit) only the soft limit. */
static int set_soft_limit(int resource, rlim_t value)
{
    struct rlimit rl;
    if (getrlimit(resource, &rl) != 0) return -1;
    if (rl.rlim_max != RLIM_INFINITY && value > rl.rlim_max) return -1;
    rl.rlim_cur = value;
    return setrlimit(resource, &rl);
}

static int hard_limit_below(int resource, rlim_t need)
{
    struct rlimit rl;
    if (getrlimit(resource, &rl) != 0) return 1;
    return rl.rlim_max != RLIM_INFINITY && rl.rlim_max < need;
}

/* A CRITICAL thread under RLIMIT_RTPRIO 5 gets SCHED_FIFO 5, not SCHED_OTHER. */
static int case_thread_rt_clamped_to_rlimit(void)
{
    if (geteuid() == 0) { printf("  root: RLIMIT_RTPRIO does not bind\n"); return CASE_SKIP; }
    if (hard_limit_below(RLIMIT_RTPRIO, 5)) { printf("  hard RLIMIT_RTPRIO < 5\n"); return CASE_SKIP; }
    if (set_soft_limit(RLIMIT_RTPRIO, 5) != 0) { perror("  setrlimit RTPRIO"); return CASE_FAIL; }

    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    int policy = -1;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    pthread_getschedparam(pthread_self(), &policy, &sp);
    if (policy != SCHED_FIFO || sp.sched_priority != 5) {
        printf("  got policy %d priority %d, want SCHED_FIFO (%d) priority 5\n",
               policy, sp.sched_priority, SCHED_FIFO);
        return CASE_FAIL;
    }
    return CASE_PASS;
}

/* No realtime allowed and RLIMIT_NICE 25 (floor -5): CRITICAL lands on nice -5,
 * not on the untouched 0 that a refused nice -15 used to leave behind. */
static int case_thread_nice_clamped_to_floor(void)
{
    if (geteuid() == 0) { printf("  root: RLIMIT_NICE does not bind\n"); return CASE_SKIP; }
    if (hard_limit_below(RLIMIT_NICE, 25)) { printf("  hard RLIMIT_NICE < 25\n"); return CASE_SKIP; }
    if (set_soft_limit(RLIMIT_RTPRIO, 0) != 0) { perror("  setrlimit RTPRIO"); return CASE_FAIL; }
    if (set_soft_limit(RLIMIT_NICE, 25) != 0) { perror("  setrlimit NICE"); return CASE_FAIL; }

    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    errno = 0;
    int nice_now = getpriority(PRIO_PROCESS, (id_t)self_tid());
    int policy = sched_getscheduler(0);
    if (policy != SCHED_OTHER || nice_now != -5) {
        printf("  got policy %d nice %d, want SCHED_OTHER nice -5\n", policy, nice_now);
        return CASE_FAIL;
    }
    return CASE_PASS;
}

/* proc_set_priority(CRITICAL) takes the same clamp on its realtime branch. */
static int case_proc_rt_clamped_to_rlimit(void)
{
    if (geteuid() == 0) { printf("  root: RLIMIT_RTPRIO does not bind\n"); return CASE_SKIP; }
    if (hard_limit_below(RLIMIT_RTPRIO, 5)) { printf("  hard RLIMIT_RTPRIO < 5\n"); return CASE_SKIP; }
    if (set_soft_limit(RLIMIT_RTPRIO, 5) != 0) { perror("  setrlimit RTPRIO"); return CASE_FAIL; }

    proc_set_priority(PROC_PRIORITY_CRITICAL);

    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    int policy = sched_getscheduler(0);
    sched_getparam(0, &sp);
    if (policy != SCHED_FIFO || sp.sched_priority != 5) {
        printf("  got policy %d priority %d, want SCHED_FIFO (%d) priority 5\n",
               policy, sp.sched_priority, SCHED_FIFO);
        return CASE_FAIL;
    }
    return CASE_PASS;
}

/* A thread that became SCHED_FIFO (directly, or by being created from one --
 * pthread_create copies the creator's policy) and then asks for ABOVE must end
 * up in the normal class at nice -5. Setting only the nice would leave it
 * SCHED_FIFO, where nice means nothing: that is how the display thread, started
 * from the promoted render thread, kept outranking nothing but everything. */
static int case_thread_leaves_realtime_for_lower_level(void)
{
    if (geteuid() == 0) { printf("  root: limits do not bind\n"); return CASE_SKIP; }
    if (hard_limit_below(RLIMIT_RTPRIO, 5)) { printf("  hard RLIMIT_RTPRIO < 5\n"); return CASE_SKIP; }
    if (hard_limit_below(RLIMIT_NICE, 25)) { printf("  hard RLIMIT_NICE < 25\n"); return CASE_SKIP; }
    if (set_soft_limit(RLIMIT_RTPRIO, 5) != 0) { perror("  setrlimit RTPRIO"); return CASE_FAIL; }
    if (set_soft_limit(RLIMIT_NICE, 25) != 0) { perror("  setrlimit NICE"); return CASE_FAIL; }

    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    if (sched_getscheduler(0) != SCHED_FIFO) {
        printf("  precondition: CRITICAL did not give SCHED_FIFO\n");
        return CASE_FAIL;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);

    errno = 0;
    int nice_now = getpriority(PRIO_PROCESS, (id_t)self_tid());
    int policy = sched_getscheduler(0);
    if (policy != SCHED_OTHER || nice_now != -5) {
        printf("  got policy %d nice %d, want SCHED_OTHER nice -5\n", policy, nice_now);
        return CASE_FAIL;
    }
    return CASE_PASS;
}

/* gui_app_start_capture promotes its caller (the render thread) so transport
 * threads spawned during device open inherit SCHED_FIFO, then hands the caller
 * back with thrd_leave_realtime(): normal class again, with the nice that
 * proc_set_priority gave it left alone. */
static int case_leave_realtime_keeps_nice(void)
{
    if (geteuid() == 0) { printf("  root: limits do not bind\n"); return CASE_SKIP; }
    if (hard_limit_below(RLIMIT_RTPRIO, 5)) { printf("  hard RLIMIT_RTPRIO < 5\n"); return CASE_SKIP; }
    if (hard_limit_below(RLIMIT_NICE, 25)) { printf("  hard RLIMIT_NICE < 25\n"); return CASE_SKIP; }
    if (set_soft_limit(RLIMIT_RTPRIO, 5) != 0) { perror("  setrlimit RTPRIO"); return CASE_FAIL; }
    if (set_soft_limit(RLIMIT_NICE, 25) != 0) { perror("  setrlimit NICE"); return CASE_FAIL; }

    if (thrd_is_realtime()) { printf("  a fresh process reports realtime\n"); return CASE_FAIL; }
    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    if (setpriority(PRIO_PROCESS, (id_t)self_tid(), -3) != 0) { perror("  setpriority -3"); return CASE_FAIL; }
    if (!thrd_is_realtime()) { printf("  CRITICAL thread does not report realtime\n"); return CASE_FAIL; }

    thrd_leave_realtime();

    errno = 0;
    int nice_now = getpriority(PRIO_PROCESS, (id_t)self_tid());
    if (thrd_is_realtime() || sched_getscheduler(0) != SCHED_OTHER || nice_now != -3) {
        printf("  got policy %d nice %d, want SCHED_OTHER nice -3\n", sched_getscheduler(0), nice_now);
        return CASE_FAIL;
    }
    return CASE_PASS;
}

/* Best-effort level 1 is (2 << 13) | 1 = 16385 in the kernel's encoding. It
 * applies to this thread, is inherited by a child spawned from it, and the
 * saved value restores exactly (PR B restores the render thread this way). */
static int case_io_priority_set_restore_inherit(void)
{
    const int want = 16385;
    int saved = thrd_get_io_priority();
    if (saved < 0) { perror("  thrd_get_io_priority"); return CASE_FAIL; }

    if (thrd_set_io_priority(THRD_IOPRIO(THRD_IOPRIO_CLASS_BE, 1)) != 0) {
        perror("  thrd_set_io_priority");
        return CASE_FAIL;
    }
    if (ioprio_of(self_tid()) != want) {
        printf("  thread ioprio %d, want %d\n", ioprio_of(self_tid()), want);
        return CASE_FAIL;
    }

    pid_t child = 0;
    char *argv[] = { "sleep", "5", NULL };
    int rc = posix_spawnp(&child, "sleep", NULL, NULL, argv, environ);
    if (rc != 0) { printf("  posix_spawnp sleep: %s\n", strerror(rc)); return CASE_FAIL; }
    int child_prio = ioprio_of(child);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    if (child_prio != want) {
        printf("  spawned child ioprio %d, want %d\n", child_prio, want);
        return CASE_FAIL;
    }

    if (thrd_set_io_priority(saved) != 0) { perror("  restore"); return CASE_FAIL; }
    if (ioprio_of(self_tid()) != saved) {
        printf("  restored ioprio %d, want the saved %d\n", ioprio_of(self_tid()), saved);
        return CASE_FAIL;
    }
    return CASE_PASS;
}

static int failures = 0;

static void run_case(const char *name, int (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); failures++; return; }
    if (pid == 0) {
        int rc = fn();
        fflush(stdout);
        _exit(rc);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : CASE_FAIL;
    printf("%s: %s\n", rc == CASE_PASS ? "PASS" : (rc == CASE_SKIP ? "SKIP" : "FAIL"), name);
    if (rc != CASE_PASS && rc != CASE_SKIP) failures++;
}

int main(void)
{
    run_case("thread realtime clamped to RLIMIT_RTPRIO", case_thread_rt_clamped_to_rlimit);
    run_case("thread nice clamped to the RLIMIT_NICE floor", case_thread_nice_clamped_to_floor);
    run_case("process realtime clamped to RLIMIT_RTPRIO", case_proc_rt_clamped_to_rlimit);
    run_case("thread leaves realtime for a lower level", case_thread_leaves_realtime_for_lower_level);
    run_case("leaving realtime keeps the nice", case_leave_realtime_keeps_nice);
    run_case("I/O priority set, restored and inherited", case_io_priority_set_restore_inherit);
    return failures == 0 ? 0 : 1;
}
