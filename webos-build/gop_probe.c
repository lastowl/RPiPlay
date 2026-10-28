/*
 * gop_probe — staged probe for MStar GOP/GWIN transparency on LG webOS 2.x
 *
 * dlopens libkadaptor.so (the MStar SDK monolith that exports MApi_GOP_GWIN_*)
 * and calls one API at a time with a SIGSEGV/SIGBUS guard, unbuffered output,
 * and a step marker before each call — so the last printed STEP line is always
 * the exact function that faulted.
 *
 * Usage:
 *   gop_probe i                         # info: GOP/GWIN state
 *   gop_probe en  <win> <0|1>           # MApi_GOP_GWIN_Enable(win, on)
 *   gop_probe blend <win> <0|1> <coef>  # MApi_GOP_GWIN_SetBlending(win, on, coef)
 *   gop_probe alpha <val>               # MApi_GOP_GWIN_SetAlphaValue(val)
 *   gop_probe ainv  <win> <0|1>         # MApi_GOP_GWIN_SetAlphaInverse(win, on)
 *   gop_probe malpha <win> <0|1>        # MApi_GOP_GWIN_EnableMultiAlpha(win, on)
 *   gop_probe transclr <win> <0|1>      # MApi_GOP_GWIN_EnableTransClr(win, on)
 *   gop_probe key <win> <r> <g> <b>     # MApi_GOP_GWIN_SetFMT0TransClr(win, r, g, b)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>

static sigjmp_buf jb;
static volatile int in_call = 0;
static const char *step = "init";

static void segv_handler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    fprintf(stderr, "!! FAULT sig=%d in [%s] PC=%p addr=%p\n", sig, step,
            (void *)(unsigned long)uc->uc_mcontext.arm_pc, info->si_addr);
    if (in_call)
        siglongjmp(jb, 1);
    _exit(139);
}

typedef unsigned char MS_U8;
typedef unsigned int  MS_U32;

static void *H;

static void *sym(const char *name)
{
    void *p = dlsym(H, name);
    printf("  sym %-40s %p\n", name, p);
    fflush(stdout);
    return p;
}

/* call a 1-u8 func: returns 0 on success, -1 on no-sym, -2 on fault */
static int call1(const char *n, MS_U8 a, MS_U8 *out)
{
    MS_U8 (*f)(MS_U8);
    step = n;
    void *p = sym(n);
    if (!p) return -1;
    f = (MS_U8 (*)(MS_U8))p;
    in_call = 1;
    if (sigsetjmp(jb, 1) == 0) {
        MS_U8 r = f(a);
        in_call = 0;
        if (out) *out = r;
        printf("  OK %s(%u) = %u\n", n, a, r);
    } else {
        in_call = 0;
        printf("  FAULT in %s(%u)\n", n, a);
        return -2;
    }
    fflush(stdout);
    return 0;
}

/* call a 2-u8 func */
static int call2(const char *n, MS_U8 a, MS_U8 b)
{
    MS_U8 (*f)(MS_U8, MS_U8);
    step = n;
    void *p = sym(n);
    if (!p) return -1;
    f = (MS_U8 (*)(MS_U8, MS_U8))p;
    in_call = 1;
    if (sigsetjmp(jb, 1) == 0) {
        MS_U8 r = f(a, b);
        in_call = 0;
        printf("  OK %s(%u, %u) = %u\n", n, a, b, r);
    } else {
        in_call = 0;
        printf("  FAULT in %s(%u, %u)\n", n, a, b);
        return -2;
    }
    fflush(stdout);
    return 0;
}

/* call a 3-u8 func */
static int call3(const char *n, MS_U8 a, MS_U8 b, MS_U8 c)
{
    MS_U8 (*f)(MS_U8, MS_U8, MS_U8);
    step = n;
    void *p = sym(n);
    if (!p) return -1;
    f = (MS_U8 (*)(MS_U8, MS_U8, MS_U8))p;
    in_call = 1;
    if (sigsetjmp(jb, 1) == 0) {
        MS_U8 r = f(a, b, c);
        in_call = 0;
        printf("  OK %s(%u, %u, %u) = %u\n", n, a, b, c, r);
    } else {
        in_call = 0;
        printf("  FAULT in %s(%u, %u, %u)\n", n, a, b, c);
        return -2;
    }
    fflush(stdout);
    return 0;
}

/* call a 4-u8 func (color key guess) */
static int call4(const char *n, MS_U8 a, MS_U8 b, MS_U8 c, MS_U8 d)
{
    MS_U8 (*f)(MS_U8, MS_U8, MS_U8, MS_U8);
    step = n;
    void *p = sym(n);
    if (!p) return -1;
    f = (MS_U8 (*)(MS_U8, MS_U8, MS_U8, MS_U8))p;
    in_call = 1;
    if (sigsetjmp(jb, 1) == 0) {
        MS_U8 r = f(a, b, c, d);
        in_call = 0;
        printf("  OK %s(%u, %u, %u, %u) = %u\n", n, a, b, c, d, r);
    } else {
        in_call = 0;
        printf("  FAULT in %s(%u, %u, %u, %u)\n", n, a, b, c, d);
        return -2;
    }
    fflush(stdout);
    return 0;
}

/* call a 1-u32 func (SetAlphaValue guess) */
static int call_u32(const char *n, MS_U32 a)
{
    MS_U8 (*f)(MS_U32);
    step = n;
    void *p = sym(n);
    if (!p) return -1;
    f = (MS_U8 (*)(MS_U32))p;
    in_call = 1;
    if (sigsetjmp(jb, 1) == 0) {
        MS_U8 r = f(a);
        in_call = 0;
        printf("  OK %s(%u) = %u\n", n, a, r);
    } else {
        in_call = 0;
        printf("  FAULT in %s(%u)\n", n, a);
        return -2;
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    if (argc < 2) {
        fprintf(stderr, "usage: %s i|en|blend|alpha|ainv|malpha|transclr|key [args]\n", argv[0]);
        return 2;
    }

    step = "dlopen";
    H = dlopen("libkadaptor.so.1.0.0", RTLD_LAZY);
    if (!H) H = dlopen("libkadaptor.so.1", RTLD_LAZY);
    if (!H) H = dlopen("/usr/lib/libkadaptor.so.1.0.0", RTLD_LAZY);
    if (!H) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }
    printf("dlopen OK %p\n", H);

    const char *mode = argv[1];

    if (!strcmp(mode, "i")) {
        MS_U8 total = 0, maxgop = 0, cur = 0;
        int r;
        r = call1("MApi_GOP_GWIN_GetTotalGwinNum", 0, &total);
        printf("GetTotalGwinNum -> r=%d total=%u\n", r, total);
        call1("MApi_GOP_GWIN_GetMaxGOPNum", 0, &maxgop);
        call1("MApi_GOP_GWIN_GetCurrentGOP", 0, &cur);
        if (r == 0 && total > 0) {
            typedef MS_U8 (*IsEn)(MS_U8);
            void *p = dlsym(H, "MApi_GOP_GWIN_IsGWINEnabled");
            if (!p) p = dlsym(H, "MApi_GOP_GWIN_IsEnabled");
            if (p) {
                IsEn f = (IsEn)p;
                for (MS_U8 w = 0; w < total && w < 32; w++) {
                    in_call = 1;
                    if (sigsetjmp(jb, 1) == 0) {
                        MS_U8 en = f(w);
                        in_call = 0;
                        MS_U8 av = 255;
                        call1("MApi_GOP_GWIN_GetAlphaValue", w, &av);
                        printf("GWIN %2u: enabled=%u alpha=%u\n", w, en, av);
                    } else {
                        in_call = 0;
                        printf("GWIN %2u: IsEnabled FAULT — stop\n", w);
                        break;
                    }
                }
            } else {
                printf("no IsGWINEnabled symbol\n");
            }
        }
    } else if (!strcmp(mode, "en") && argc >= 4) {
        call2("MApi_GOP_GWIN_Enable", (MS_U8)atoi(argv[2]), (MS_U8)atoi(argv[3]));
    } else if (!strcmp(mode, "blend") && argc >= 5) {
        call3("MApi_GOP_GWIN_SetBlending", (MS_U8)atoi(argv[2]), (MS_U8)atoi(argv[3]), (MS_U8)atoi(argv[4]));
    } else if (!strcmp(mode, "alpha") && argc >= 3) {
        call_u32("MApi_GOP_GWIN_SetAlphaValue", (MS_U32)strtoul(argv[2], NULL, 0));
    } else if (!strcmp(mode, "ainv") && argc >= 4) {
        call2("MApi_GOP_GWIN_SetAlphaInverse", (MS_U8)atoi(argv[2]), (MS_U8)atoi(argv[3]));
    } else if (!strcmp(mode, "malpha") && argc >= 4) {
        call2("MApi_GOP_GWIN_EnableMultiAlpha", (MS_U8)atoi(argv[2]), (MS_U8)atoi(argv[3]));
    } else if (!strcmp(mode, "transclr") && argc >= 4) {
        call2("MApi_GOP_GWIN_EnableTransClr", (MS_U8)atoi(argv[2]), (MS_U8)atoi(argv[3]));
    } else if (!strcmp(mode, "key") && argc >= 6) {
        call4("MApi_GOP_GWIN_SetFMT0TransClr", (MS_U8)atoi(argv[2]), (MS_U8)atoi(argv[3]),
              (MS_U8)atoi(argv[4]), (MS_U8)atoi(argv[5]));
    } else {
        fprintf(stderr, "bad args for mode '%s'\n", mode);
        return 2;
    }

    printf("done\n");
    return 0;
}
