// segvbt: LD_PRELOAD that prints a backtrace to stderr when a process crashes. gdb and strace
// can't attach under proot (it is already the tracer), so this is how a crash in the session
// (KWin, plasmashell) leaves a trace in /tmp/plasma-mobile-session.log. After printing, the
// default action runs, so the process still dies (and dumps core) as it would have.
#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void report(int sig, siginfo_t *info, void *context) {
    (void)context;
    char line[160];
    int n = snprintf(line, sizeof line, "segvbt: pid %d got signal %d (%s) at address %p\n",
                     (int)getpid(), sig, strsignal(sig), info ? info->si_addr : NULL);
    if (n > 0) (void)!write(STDERR_FILENO, line, (size_t)n);
    void *frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor)) static void install(void) {
    // backtrace() loads libgcc on first use, which must not happen inside the handler.
    void *warm[1];
    backtrace(warm, 1);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = report;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof signals / sizeof *signals; i++) {
        struct sigaction old;
        // Leave handlers the program installed itself (none exist yet at preload time, but be safe).
        if (sigaction(signals[i], NULL, &old) == 0 && old.sa_handler == SIG_DFL)
            sigaction(signals[i], &action, NULL);
    }
}
