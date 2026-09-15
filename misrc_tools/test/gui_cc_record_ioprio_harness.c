/*
 * Caption-recorder child I/O class harness (Linux only; compiled and run by
 * ci_guard_tests.py together with gui_cc_record.c, standalone -- see
 * gui_cc_record_argv_harness.c for why that module has no project includes).
 *
 * gui_cc_record_start() spawns ffmpeg from the render thread. The child must
 * come up in best-effort I/O level 1 -- one step below the RF writers, which
 * take level 0 -- and the spawning thread must keep its own class, or the
 * render thread would carry the recorder's class from then on. A stand-in
 * script plays ffmpeg: it answers the -devices / -muxers probes and otherwise
 * sleeps, so no capture device and no real ffmpeg are involved.
 */
#include "gui_cc_record.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int ioprio_of(int who)
{
    return (int)syscall(SYS_ioprio_get, 1 /* IOPRIO_WHO_PROCESS */, who);
}

int main(void)
{
    char dir[] = "/tmp/misrc_cc_ioprio_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char stub[256], out[256], log[300];
    snprintf(stub, sizeof(stub), "%s/ffmpeg", dir);
    snprintf(out, sizeof(out), "%s/captions.scc", dir);
    snprintf(log, sizeof(log), "%s.ffmpeg.log", out);

    FILE *f = fopen(stub, "w");
    if (!f) { perror("fopen stub"); return 1; }
    fputs("#!/bin/sh\n"
          "case \"$*\" in\n"
          "  *-devices*) echo ' D  v4l2vbi         raw VBI'; exit 0 ;;\n"
          "  *-muxers*)  echo '  E scc             Scenarist Closed Captions'; exit 0 ;;\n"
          "esac\n"
          "exec sleep 30\n", f);
    fclose(f);
    chmod(stub, 0755);

    const int want = 16385;  /* (2 << 13) | 1 = 16384 + 1: best-effort class, level 1 */
    int caller_before = ioprio_of(0);

    gui_cc_record_set_ffmpeg(stub);
    char err[256];
    int rc = gui_cc_record_start("/dev/null", out, err, sizeof(err));
    int pid = gui_cc_record_get_status().child_pid;
    int child_io = (rc == 0 && pid > 0) ? ioprio_of(pid) : -1;
    int caller_after = ioprio_of(0);
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    unlink(stub); unlink(out); unlink(log); rmdir(dir);

    if (rc != 0) { printf("FAIL: gui_cc_record_start: %s\n", err); return 1; }
    int bad = 0;
    if (child_io != want) {
        printf("FAIL: caption ffmpeg child I/O priority %d, want %d (best-effort 1)\n",
               child_io, want);
        bad = 1;
    }
    if (caller_after != caller_before) {
        printf("FAIL: spawning thread's I/O priority is %d after the spawn, was %d\n",
               caller_after, caller_before);
        bad = 1;
    }
    if (!bad) printf("PASS: caption child best-effort 1, spawning thread unchanged\n");
    return bad;
}
