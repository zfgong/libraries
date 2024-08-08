/******************************************************************************
 * Copyright (C) 2014-2020 Zhifeng Gong <gozfree@163.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/
#ifndef __cplusplus
#define _GNU_SOURCE
#endif
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#if !defined(__UCLIBC__)
#include <execinfo.h>
#endif

#include "libdebug.h"

#define BT_SIZE 100
#define CMD_SIZE 1024

/*
 * from /usr/include/bits/signum.h
 */
static int signals_all[] = {
    SIGHUP,    /* 1 Hangup (POSIX).  */
    SIGINT,    /* 2 Interrupt (ANSI).  */
    SIGQUIT,   /* 3 Quit (POSIX).  */
    SIGILL,    /* 4 Illegal instruction (ANSI).  */
    SIGTRAP,   /* 5 Trace trap (POSIX).  */
    SIGABRT,   /* 6 Abort (ANSI).  */
    SIGIOT,    /* 6 IOT trap (4.2 BSD).  */
    SIGBUS,    /* 7 BUS error (4.2 BSD).  */
    SIGFPE,    /* 8 Floating-point exception (ANSI).  */
    SIGKILL,   /* 9 Kill, unblockable (POSIX).  */
    SIGUSR1,   /* 10 User-defined signal 1 (POSIX).  */
    SIGSEGV,   /* 11 Segmentation violation (ANSI).  */
    SIGUSR2,   /* 12 User-defined signal 2 (POSIX).  */
    SIGPIPE,   /* 13 Broken pipe (POSIX).  */
    SIGALRM,   /* 14 Alarm clock (POSIX).  */
    SIGTERM,   /* 15 Termination (ANSI).  */
    SIGSTKFLT, /* 16 Stack fault.  */
    SIGCLD,    /* 17 Same as SIGCHLD (System V).  */
    SIGCHLD,   /* 17 Child status has changed (POSIX).  */
    SIGCONT,   /* 18 Continue (POSIX).  */
    SIGSTOP,   /* 19 Stop, unblockable (POSIX).  */
    SIGTSTP,   /* 20 Keyboard stop (POSIX).  */
    SIGTTIN,   /* 21 Background read from tty (POSIX).  */
    SIGTTOU,   /* 22 Background write to tty (POSIX).  */
    SIGURG,    /* 23 Urgent condition on socket (4.2 BSD).  */
    SIGXCPU,   /* 24 CPU limit exceeded (4.2 BSD).  */
    SIGXFSZ,   /* 25 File size limit exceeded (4.2 BSD).  */
    SIGVTALRM, /* 26 Virtual alarm clock (4.2 BSD).  */
    SIGPROF,   /* 27 Profiling alarm clock (4.2 BSD).  */
    SIGWINCH,  /* 28 Window size change (4.3 BSD, Sun).  */
    SIGPOLL,   /* 29 Pollable event occurred (System V).  */
    SIGIO,     /* 29 I/O now possible (4.2 BSD).  */
    SIGPWR,    /* 30 Power failure restart (System V).  */
    SIGSYS,    /* 31 Bad system call.  */
};

static int signals_trace[] =
{
    SIGILL,  /* Illegal instruction (ANSI).  */
    SIGABRT, /* Abort (ANSI).  */
    SIGBUS,  /* BUS error (4.2 BSD). (unaligned access) */
    SIGFPE,  /* Floating-point exception (ANSI).  */
    SIGSEGV, /* Segmentation violation (ANSI).  */
};

#if !defined(__UCLIBC__)
static void *get_uc_mcontext_pc(ucontext_t *uc)
{
#if defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
    /* OSX < 10.6 */
    #if defined(__x86_64__)
    return (void*) uc->uc_mcontext->__ss.__rip;
    #elif defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__eip;
    #else
    return (void*) uc->uc_mcontext->__ss.__srr0;
    #endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
    /* OSX >= 10.6 */
    #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__rip;
    #else
    return (void*) uc->uc_mcontext->__ss.__eip;
    #endif
#elif defined(__linux__)
    /* Linux */
    #if defined(__i386__)
    return (void*) uc->uc_mcontext.gregs[REG_EIP]; /* Linux 32 */
    #elif defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[REG_RIP]; /* Linux 64 */
    #elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
    #elif defined(__arm__)
    return (void*) uc->uc_mcontext.arm_pc;
    #elif defined(__aarch64__)
    return (void*) uc->uc_mcontext.pc;
    #else
    return NULL;
    #endif
#else
    return NULL;
#endif
}
#endif

#if !defined(__UCLIBC__)
static void backtrace_symbols_detail(void *array[], int size)
{
    int i;
    char cmd[CMD_SIZE] = "addr2line -C -f -e ";
    char* prog = cmd + strlen(cmd);
    int r = readlink("/proc/self/exe", prog, sizeof(cmd) - (prog-cmd)-1);
    if (r == -1) {
        fprintf(stderr, "backtrace_symbols_detail unsupported!\n");
        perror("readlink");
        return;
    }
    prog[r] = '\0';
    FILE* fp = popen(cmd, "w");
    if (!fp) {
        perror("popen");
        return;
    }
    for (i = 1; i < size; ++i) {//from 1, ignore this file info
        fprintf(fp, "%p\n", array[i]);
    }
    pclose(fp);
}
#endif

static void backtrace_handler(int sig_num, siginfo_t *info, void *ucontext)
{
#if !defined(__UCLIBC__)
    void *array[BT_SIZE];
    int i, size = 0;
    char **strings = NULL;
    void *caller_addr = get_uc_mcontext_pc((ucontext_t *)ucontext);
    fprintf(stderr, "Program received signal %s.\n", strsignal(sig_num));
    fprintf(stderr, "%d:%s [%p]\n", sig_num, strerror(sig_num), caller_addr);

    size = backtrace(array, BT_SIZE);
    /* overwrite sigaction with caller's address, but seems no needed */
    array[1] = caller_addr;
    strings = backtrace_symbols(array, size);
    for (i = 0; i < size; ++ i) {
        fprintf(stderr, "#%d  %s\n", i, strings[i]);
    }
    backtrace_symbols_detail(array, size);
#endif

    exit(EXIT_FAILURE);
}

int debug_backtrace_init(void)
{
    int i;
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_sigaction = backtrace_handler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    int ret = 0;
    for (i = 0; i < (sizeof(signals_trace) / sizeof(int)); ++i) {
        if (sigaction(signals_trace[i], &sa, NULL) != 0) {
            fprintf(stderr, "backtrace failed to set signal handler for %s(%d)!\n",
                    strsignal(signals_trace[i]), signals_trace[i]);
            ret = -1;
            break;
        }
    }
    return ret;
}

void debug_backtrace_dump(void)
{
#if !defined(__UCLIBC__)
    void* buffer[BT_SIZE];
    int size = backtrace(buffer, BT_SIZE);
    backtrace_symbols_detail(buffer, size);
#endif
}

static void debug_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{
    fprintf(stderr, "signal %d (%s) received from %d\n",
                      signo, strsignal(signo), siginfo->si_pid);

    switch (signo) {
    case SIGHUP:
        break;
    case SIGINT:
        break;
    case SIGQUIT:
        break;
    case SIGILL:
        break;
    case SIGTRAP:
        break;
    case SIGABRT://same as SIGIOT:
        break;
    case SIGBUS:
        break;
    case SIGFPE:
        break;
    case SIGKILL:
        break;
    case SIGUSR1:
        break;
    case SIGSEGV:
        signal(signo, SIG_IGN);
        break;
    case SIGUSR2:
        break;
    case SIGPIPE:
        break;
    case SIGALRM:
        break;
    case SIGTERM:
        break;
    case SIGSTKFLT:
        break;
    case SIGCLD://same as SIGCHLD
        break;
    case SIGCONT:
        break;
    case SIGSTOP:
        break;
    case SIGTSTP:
        break;
    case SIGTTIN:
        break;
    case SIGTTOU:
        break;
    case SIGURG:
        break;
    case SIGXCPU:
        break;
    case SIGXFSZ:
        break;
    case SIGVTALRM:
        break;
    case SIGPROF:
        break;
    case SIGWINCH:
        break;
    case SIGPOLL://same as SIGIO
        break;
    case SIGPWR:
        break;
    case SIGSYS://same as SIGUNUSED
        break;
    default:
        break;
    }
}

int debug_signals_init(void)
{
    int i;
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_sigaction = debug_signal_handler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    for (i = 0; i < (sizeof(signals_all) / sizeof(int)); ++i) {
        if (sigaction(signals_all[i], &sa, NULL) == -1) {
            fprintf(stderr, "signal failed to set signal handler for %s(%d)!\n",
                  strsignal(signals_all[i]), signals_all[i]);
        }
    }

    return 0;
}
