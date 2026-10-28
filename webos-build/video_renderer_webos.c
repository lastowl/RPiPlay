/*
 * Video renderer for LG webOS TVs using GStreamer with mvsink
 * Based on RPiPlay's video_renderer_gstreamer.c
 */

#include "video_renderer.h"
#include <assert.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <string.h>
#include <setjmp.h>
static void launch_transparent_app(void);
static void setup_video_display(void);
static void set_pipeline_foreground(void);

static void term_handler(int sig) {
    _exit(0);
}

static void usr1_handler(int sig) {
    (void)sig;
    setup_video_display();
}

static void crash_handler(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);
    fprintf(stderr, "  PC = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_pc);
    fprintf(stderr, "  LR = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_lr);
    fprintf(stderr, "  SP = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_sp);
    fprintf(stderr, "  R0 = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_r0);
    fflush(stderr);

    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[256];
        fprintf(stderr, "\n=== MAPS (text segments near PC) ===\n");
        unsigned long pc = uc->uc_mcontext.arm_pc;
        while (fgets(line, sizeof(line), f)) {
            unsigned long start, end;
            if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                if (pc >= start && pc < end) {
                    fprintf(stderr, ">>> %s", line);
                }
            }
        }
        fclose(f);
        fflush(stderr);
    }
    _exit(128 + sig);
}

/* MStar API typedefs */
typedef unsigned char MS_U8;
typedef unsigned short MS_U16;
typedef unsigned int MS_U32;
typedef unsigned char MS_BOOL;

/* XC (Scaler) API function types */
typedef void (*fn_XC_EnableMainWindow)(MS_BOOL);
typedef MS_BOOL (*fn_XC_IsBlackVideoEnable)(void);
typedef void (*fn_XC_GenerateBlackVideo)(MS_BOOL, MS_U8);
typedef void (*fn_XC_GenerateBlackVideoForBothWin)(MS_BOOL);
typedef void (*fn_XC_SetVideoOnOSD)(MS_BOOL, MS_U8);
typedef void (*fn_XC_SetGstreamerProcess)(MS_BOOL);
typedef MS_BOOL (*fn_MVOP_Enable)(MS_BOOL);

static sigjmp_buf gop_jmpbuf;
static volatile int gop_jump_active = 0;

static void gop_segv_handler(int sig, siginfo_t *info, void *ctx) {
    if (gop_jump_active) {
        gop_jump_active = 0;
        siglongjmp(gop_jmpbuf, sig);
    }
    crash_handler(sig, info, ctx);
}

#define GOP_TRY(label) do { \
    gop_jump_active = 1; \
    if (sigsetjmp(gop_jmpbuf, 1) != 0) { \
        printf("[webos-video] GOP: %s crashed, continuing\n", label); \
        fflush(stdout); \
        gop_jump_active = 0; \
        break; \
    }

#define GOP_ENDTRY gop_jump_active = 0; } while(0)

static void set_pipeline_foreground(void) {
    printf("[webos-video] setting pipeline foreground via luna-send...\n");
    fflush(stdout);

    /* Get our pipeline ID from umediaserver */
    FILE *fp = popen(
        "luna-send -n 1 -f -w 5000 luna://com.webos.media/getActivePipelines '{}' 2>/dev/null",
        "r");
    if (!fp) {
        printf("[webos-video] popen for getActivePipelines failed\n");
        fflush(stdout);
        return;
    }

    char buf[4096];
    char pipeline_id[128] = {0};
    int my_pid = getpid();

    /* Read all output and find our pipeline by PID */
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        size_t n = fread(buf + total, 1, sizeof(buf) - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    pclose(fp);

    printf("[webos-video] getActivePipelines: %s\n", buf);
    fflush(stdout);

    /* Parse out pipeline ID matching our PID - simple string search */
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "\"pid\": %d", my_pid);
    /* Also try without space */
    char pid_str2[32];
    snprintf(pid_str2, sizeof(pid_str2), "\"pid\":%d", my_pid);

    char *pid_pos = strstr(buf, pid_str);
    if (!pid_pos) pid_pos = strstr(buf, pid_str2);

    if (pid_pos) {
        /* Found our PID, now search backwards for "id" field */
        char *search = pid_pos;
        while (search > buf) {
            search--;
            if (strncmp(search, "\"id\":", 5) == 0 || strncmp(search, "\"id\": ", 6) == 0) {
                /* Find the quoted value */
                char *q1 = strchr(search + 4, '"');
                if (q1) {
                    q1++;
                    char *q2 = strchr(q1, '"');
                    if (q2 && (q2 - q1) < (int)sizeof(pipeline_id) - 1) {
                        memcpy(pipeline_id, q1, q2 - q1);
                        pipeline_id[q2 - q1] = '\0';
                    }
                }
                break;
            }
        }
    }

    if (pipeline_id[0] == '\0') {
        /* Fallback: just find the first media pipeline */
        char *id_pos = strstr(buf, "\"id\":");
        if (id_pos) {
            char *q1 = strchr(id_pos + 4, '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2 && (q2 - q1) < (int)sizeof(pipeline_id) - 1) {
                    memcpy(pipeline_id, q1, q2 - q1);
                    pipeline_id[q2 - q1] = '\0';
                }
            }
        }
    }

    if (pipeline_id[0] == '\0') {
        printf("[webos-video] could not find our pipeline ID\n");
        fflush(stdout);
        return;
    }

    printf("[webos-video] our pipeline: %s, setting foreground...\n", pipeline_id);
    fflush(stdout);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "luna-send -n 1 -f -w 5000 luna://com.webos.media/notifyForeground "
        "'{\"connectionId\":\"%s\", \"foreground\": true}' 2>/dev/null",
        pipeline_id);

    fp = popen(cmd, "r");
    if (fp) {
        char result[256];
        if (fgets(result, sizeof(result), fp))
            printf("[webos-video] notifyForeground: %s", result);
        pclose(fp);
    }
    fflush(stdout);
}

static void launch_transparent_app(void) {
    printf("[webos-video] launching transparent app...\n");
    fflush(stdout);
    FILE *fp = popen(
        "luna-send -n 1 -f -w 5000 luna://com.webos.applicationManager/launch "
        "'{\"id\":\"com.rpiplay.airplay\"}' 2>/dev/null", "r");
    if (fp) {
        char result[256];
        if (fgets(result, sizeof(result), fp))
            printf("[webos-video] launch result: %s", result);
        pclose(fp);
    }
    fflush(stdout);
}

static void setup_video_display(void) {
    void *lib = dlopen("libkadaptor.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libkadaptor.so", RTLD_LAZY);
    if (!lib) lib = dlopen("libkadaptor.so.1", RTLD_LAZY);
    if (!lib) lib = dlopen("/usr/lib/libkadaptor.so.1.0.0", RTLD_LAZY);
    if (!lib) {
        printf("[webos-video] XC: cannot load libkadaptor: %s\n", dlerror());
        fflush(stdout);
        return;
    }
    printf("[webos-video] XC: loaded libkadaptor\n");
    fflush(stdout);

    struct sigaction sa_new, sa_old;
    sa_new.sa_sigaction = gop_segv_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa_new, &sa_old);

    fn_XC_EnableMainWindow pEnableMain = (fn_XC_EnableMainWindow)dlsym(lib, "MApi_XC_EnableMainWindow");
    fn_XC_IsBlackVideoEnable pIsBlack = (fn_XC_IsBlackVideoEnable)dlsym(lib, "MApi_XC_IsBlackVideoEnable");
    fn_XC_GenerateBlackVideo pGenBlack = (fn_XC_GenerateBlackVideo)dlsym(lib, "MApi_XC_GenerateBlackVideo");
    fn_XC_GenerateBlackVideoForBothWin pGenBlackBoth = (fn_XC_GenerateBlackVideoForBothWin)dlsym(lib, "MApi_XC_GenerateBlackVideoForBothWin");
    fn_XC_SetVideoOnOSD pVideoOnOSD = (fn_XC_SetVideoOnOSD)dlsym(lib, "MApi_XC_SetVideoOnOSD");
    fn_XC_SetGstreamerProcess pSetGstProc = (fn_XC_SetGstreamerProcess)dlsym(lib, "MApi_XC_Set_GstreamerProcess");
    fn_MVOP_Enable pMVOPEnable = (fn_MVOP_Enable)dlsym(lib, "MDrv_MVOP_Enable");

    printf("[webos-video] XC: GenBlack=%p IsBlack=%p EnableMain=%p VideoOnOSD=%p GstProc=%p MVOP=%p\n",
           (void*)pGenBlack, (void*)pIsBlack, (void*)pEnableMain,
           (void*)pVideoOnOSD, (void*)pSetGstProc, (void*)pMVOPEnable);
    fflush(stdout);

    /* Clear GStreamer process flag FIRST so scaler functions actually execute
     * instead of all returning "DO NOTHING" */
    if (pSetGstProc) {
        GOP_TRY("Set_GstreamerProcess(0)") {
            pSetGstProc(0);
            printf("[webos-video] XC: Set_GstreamerProcess(FALSE) done\n");
            fflush(stdout);
        } GOP_ENDTRY;
    }

    /* Disable black video */
    if (pGenBlack) {
        GOP_TRY("GenerateBlackVideo(0,MAIN)") {
            pGenBlack(0, 0);
            printf("[webos-video] XC: GenerateBlackVideo(FALSE, MAIN) done\n");
            fflush(stdout);
        } GOP_ENDTRY;
    }
    if (pGenBlackBoth) {
        GOP_TRY("GenerateBlackVideoForBothWin") {
            pGenBlackBoth(0);
            printf("[webos-video] XC: GenerateBlackVideoForBothWin(FALSE) done\n");
            fflush(stdout);
        } GOP_ENDTRY;
    }

    /* Enable the main video window */
    if (pEnableMain) {
        GOP_TRY("EnableMainWindow") {
            pEnableMain(1);
            printf("[webos-video] XC: EnableMainWindow(TRUE) done\n");
            fflush(stdout);
        } GOP_ENDTRY;
    }

    /* Enable MVOP */
    if (pMVOPEnable) {
        GOP_TRY("MVOP_Enable") {
            MS_BOOL ret = pMVOPEnable(1);
            printf("[webos-video] MVOP: Enable(TRUE) = %d\n", ret);
            fflush(stdout);
        } GOP_ENDTRY;
    }

    /* Verify black video is off */
    if (pIsBlack) {
        GOP_TRY("IsBlackVideoEnable_check") {
            MS_BOOL black = pIsBlack();
            printf("[webos-video] XC: black video after setup = %d\n", black);
            fflush(stdout);
        } GOP_ENDTRY;
    }

    sigaction(SIGSEGV, &sa_old, NULL);
    printf("[webos-video] video display setup complete\n");
    fflush(stdout);
}

typedef struct video_renderer_webos_s {
    video_renderer_t base;
    GstElement *appsrc, *pipeline, *sink;
    int frame_count;
    int gop_done;
} video_renderer_webos_t;

static const video_renderer_funcs_t video_renderer_webos_funcs;

video_renderer_t *video_renderer_gstreamer_init(logger_t *logger, video_renderer_config_t const *config) {
    video_renderer_webos_t *renderer;
    GError *error = NULL;

    renderer = calloc(1, sizeof(video_renderer_webos_t));
    assert(renderer);

    {
        struct sigaction sa;
        sa.sa_sigaction = crash_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
    }

    signal(SIGTERM, term_handler);
    signal(SIGINT, term_handler);
    signal(SIGUSR1, usr1_handler);

    printf("[webos-video] calling gst_init...\n");
    fflush(stdout);
    gst_init(NULL, NULL);
    printf("[webos-video] gst_init done\n");
    fflush(stdout);

    renderer->base.logger = logger;
    renderer->base.funcs = &video_renderer_webos_funcs;
    renderer->base.type = VIDEO_RENDERER_GSTREAMER;

    const char *pipelines[] = {
        "appsrc name=video_source stream-type=0 ! "
        "h264parse ! omxh264dec ! mvsink name=video_sink disp-x=0 disp-y=0 disp-w=1920 disp-h=1080",

        "appsrc name=video_source stream-type=0 ! "
        "h264parse ! omxh264dec ! mvsink name=video_sink disp-x=0 disp-y=0 disp-w=3840 disp-h=2160",

        "appsrc name=video_source stream-type=0 ! "
        "h264parse ! omxh264dec ! fakesink name=video_sink sync=false",

        "appsrc name=video_source ! "
        "fakesink name=video_sink sync=false",
        NULL
    };

    for (int i = 0; pipelines[i] != NULL; i++) {
        error = NULL;
        printf("[webos-video] trying pipeline %d: %s\n", i, pipelines[i]);
        fflush(stdout);
        renderer->pipeline = gst_parse_launch(pipelines[i], &error);
        if (renderer->pipeline && !error) {
            printf("[webos-video] Using pipeline %d\n", i);
            fflush(stdout);
            break;
        }
        if (error) {
            printf("[webos-video] Pipeline %d failed: %s\n", i, error->message);
            fflush(stdout);
            g_error_free(error);
        }
        if (renderer->pipeline) {
            gst_object_unref(renderer->pipeline);
            renderer->pipeline = NULL;
        }
    }

    if (!renderer->pipeline) {
        printf("[webos-video] No working pipeline found!\n");
        free(renderer);
        return NULL;
    }

    renderer->appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_source");
    renderer->sink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_sink");

    GstCaps *caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        NULL);
    if (caps) {
        printf("[webos-video] setting caps: video/x-h264 byte-stream\n");
        fflush(stdout);
        g_object_set(renderer->appsrc, "caps", caps, NULL);
        gst_caps_unref(caps);
        printf("[webos-video] caps set OK\n");
        fflush(stdout);
    }

    if (renderer->sink) {
        g_object_set(renderer->sink, "mute", 0, NULL);
        printf("[webos-video] set mvsink mute=false\n");
        fflush(stdout);
    }

    renderer->frame_count = 0;
    renderer->gop_done = 0;

    printf("[webos-video] init complete, appsrc=%p sink=%p\n",
           (void*)renderer->appsrc, (void*)renderer->sink);
    fflush(stdout);
    return &renderer->base;
}

static void video_renderer_webos_start(video_renderer_t *renderer) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    GstStateChangeReturn ret;

    /* Pipeline is created (mvsink did XC_Init, setting IsGstreamerProcess=1).
     * Clear it before PLAYING so the scaler functions execute properly
     * when mvsink sets up the display path during state transition. */
    {
        void *lib = dlopen("libkadaptor.so", RTLD_LAZY | RTLD_NOLOAD);
        if (lib) {
            struct sigaction sa_new, sa_old;
            sa_new.sa_sigaction = gop_segv_handler;
            sigemptyset(&sa_new.sa_mask);
            sa_new.sa_flags = SA_SIGINFO;
            sigaction(SIGSEGV, &sa_new, &sa_old);

            fn_XC_SetGstreamerProcess pSetGstProc =
                (fn_XC_SetGstreamerProcess)dlsym(lib, "MApi_XC_Set_GstreamerProcess");
            if (pSetGstProc) {
                GOP_TRY("early_Set_GstreamerProcess") {
                    pSetGstProc(0);
                    printf("[webos-video] cleared IsGstreamerProcess before PLAYING\n");
                    fflush(stdout);
                } GOP_ENDTRY;
            }
            sigaction(SIGSEGV, &sa_old, NULL);
        }
    }

    ret = gst_element_set_state(r->pipeline, GST_STATE_PLAYING);
    printf("[webos-video] PLAYING returned %d\n", ret);
    fflush(stdout);

    usleep(500000);
    set_pipeline_foreground();
}

static void video_renderer_webos_render_buffer(video_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts, int type) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    GstBuffer *buffer;

    if (data_len == 0) return;

    r->frame_count++;

    if (r->frame_count == 90 && !r->gop_done) {
        r->gop_done = 1;
        printf("[webos-video] %d frames received, setting up video display...\n", r->frame_count);
        fflush(stdout);
        launch_transparent_app();
        usleep(500000);
        setup_video_display();
    }

    buffer = gst_buffer_new_and_alloc(data_len);
    assert(buffer != NULL);
    GST_BUFFER_DTS(buffer) = (GstClockTime)pts;
    gst_buffer_fill(buffer, 0, data, data_len);
    gst_app_src_push_buffer(GST_APP_SRC(r->appsrc), buffer);
}

void video_renderer_webos_flush(video_renderer_t *renderer) {
}

void video_renderer_webos_destroy(video_renderer_t *renderer) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    gst_element_set_state(r->pipeline, GST_STATE_NULL);
    gst_object_unref(r->pipeline);

    if (r->gop_done) {
        printf("[webos-video] closing transparent app...\n");
        fflush(stdout);
        FILE *fp = popen(
            "luna-send -n 1 -f -w 3000 luna://com.webos.applicationManager/closeByAppId "
            "'{\"id\":\"com.rpiplay.airplay\"}' 2>/dev/null", "r");
        if (fp) pclose(fp);
    }

    free(renderer);
}

void video_renderer_webos_update_background(video_renderer_t *renderer, int type) {
}

static const video_renderer_funcs_t video_renderer_webos_funcs = {
    .start = video_renderer_webos_start,
    .render_buffer = video_renderer_webos_render_buffer,
    .flush = video_renderer_webos_flush,
    .destroy = video_renderer_webos_destroy,
    .update_background = video_renderer_webos_update_background,
};
