/* Guest-only first-boot supervisor. Copyright (c) 2026 j0shua-SYSON. MIT.
 * A scoped iPhone OS power assertion lets dpkg finish while the display sleeps.
 * No host APIs, permanent preference changes, or synthetic guest input. */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

#ifdef S5LBOX_BOOTSTRAP_TEST
#include "test_guest_bootstrap_power.h"
#define SCRIPT "fixture/install.sh"
#define COMPLETE "fixture/complete"
#define LOG "fixture/install.log"
#else
#include <CoreFoundation/CFString.h>
#include <dlfcn.h>
#define SCRIPT "/usr/libexec/s5lbox-guest-install"
#define COMPLETE "/private/var/lib/s5lbox/complete"
#define LOG "/private/var/log/s5lbox-guest-install.log"
static uint32_t assertion;
static int (*release_assertion)(uint32_t);
static int power_begin(void) {
    void *kit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!kit) return 0;
    int (*create)(CFStringRef, uint32_t, CFStringRef, uint32_t *) =
        dlsym(kit, "IOPMAssertionCreateWithName");
    release_assertion = dlsym(kit, "IOPMAssertionRelease");
    if (!create || !release_assertion) return 0;
    // powerd can still be starting when launchd first runs this job.
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        int result = create(CFSTR("NoIdleSleepAssertion"), 255,
                            CFSTR("S5LBox first-boot setup"), &assertion);
        if (!result) return 1;
        fprintf(stderr, "Guest setup power assertion unavailable: 0x%x\n", result);
        sleep(1);
    }
    return 0;
}
static void power_end(void) {
    int result = release_assertion(assertion);
    fprintf(stderr, "Guest setup sleep assertion released: 0x%x\n", result);
}
#endif

static volatile sig_atomic_t interrupted;
static void stop(int number) { interrupted = number; }

int main(void) {
#ifndef S5LBOX_BOOTSTRAP_TEST
    if (getuid() != 0) return 1;
#endif
    // Subsequent boots do not even acquire an assertion.
    if (!access(COMPLETE, F_OK)) return 0;
    int log = open(LOG, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
    if (log < 0) return 1;
    if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) return 1;
    if (log > STDERR_FILENO) close(log);
    if (!power_begin()) {
        fprintf(stderr, "Cannot protect first-boot setup from sleep; retrying\n");
        return 1; // launchd retries; do not start an unprotected transaction.
    }
    fprintf(stderr, "Guest setup sleep assertion acquired\n");
    struct sigaction handler = {0}, previous_term, previous_int;
    handler.sa_handler = stop;
    sigemptyset(&handler.sa_mask);
    sigaction(SIGTERM, &handler, &previous_term);
    sigaction(SIGINT, &handler, &previous_int);
    pid_t child = fork();
    if (child == 0) {
        sigaction(SIGTERM, &previous_term, NULL);
        sigaction(SIGINT, &previous_int, NULL);
        execl("/bin/bash", "bash", SCRIPT, (char *)NULL);
        _exit(127);
    }
    int status = 0, waited = 0;
    if (child > 0) {
        for (;;) {
            if (interrupted) { kill(child, interrupted); interrupted = 0; }
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) { waited = 1; break; }
            if (result < 0 && errno == EINTR) continue;
            // A signal between the check above and waitpid must not leave
            // shutdown waiting indefinitely for a still-running child.
            if (result == 0) { usleep(100000); continue; }
            break;
        }
    }
    power_end();
    if (!waited) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
