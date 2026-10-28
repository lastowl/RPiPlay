/*
 * Video renderer for LG webOS TVs
 * Based on RPiPlay's video_renderer_gstreamer.c
 *
 * The H.264 stream is remuxed to MPEG-TS over RTP and sent to a local UDP
 * port. The com.rpiplay.airplay web app plays that port through the TV's
 * built-in Miracast media pipeline (a <video> with a "miracast://" source),
 * so umediaserver and the compositor own decoding and the video plane,
 * exactly as for the stock Screen Share app.
 *
 * Must run in the real rootfs (not the dev-mode jail) for luna-send to work.
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
#include <string.h>
#include <time.h>
#include <pthread.h>

#define APP_ID "com.rpiplay.airplay"
#define UDP_PORT 5000

/* iOS sends an IDR only when mirroring starts (or the picture changes a
 * lot), and the TV's decoder may not be running yet when it arrives. So the
 * codec config and every frame since the last IDR are always kept, and
 * replayed ("primed") into the TV pipeline once it is listening and again a
 * few seconds later. Replaying restarts the decoder at the keyframe and ends
 * on the same reference frames the live stream continues from. */
#define GOP_MAX_BUFFERS 600
#define GOP_MAX_BYTES (8 * 1024 * 1024)
static const int prime_delays[] = { 0, 2, 5 };   /* seconds after the port is bound */
#define PRIME_COUNT (int)(sizeof(prime_delays) / sizeof(prime_delays[0]))

static volatile sig_atomic_t prime_requested = 0;

static void usr1_handler(int sig) {
    (void)sig;
    prime_requested = 1;    /* kill -USR1 <pid> replays the GOP, for debugging */
}

static void crash_handler(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);
    fprintf(stderr, "  PC = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_pc);
    fprintf(stderr, "  LR = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_lr);
    fflush(stderr);

    /* Show which libraries PC and LR fall in */
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[256];
        unsigned long pc = uc->uc_mcontext.arm_pc, lr = uc->uc_mcontext.arm_lr;
        while (fgets(line, sizeof(line), f)) {
            unsigned long start, end;
            if (sscanf(line, "%lx-%lx", &start, &end) == 2 &&
                ((pc >= start && pc < end) || (lr >= start && lr < end)))
                fprintf(stderr, ">>> %s", line);
        }
        fclose(f);
        fflush(stderr);
    }
    _exit(128 + sig);
}

static void term_handler(int sig) {
    _exit(0);
}

typedef struct video_renderer_webos_s {
    video_renderer_t base;
    GstElement *appsrc, *pipeline;
    pthread_mutex_t lock;

    int connections;
    int app_launched;
    int tv_ready;              /* TV pipeline is bound to UDP_PORT */
    time_t last_ready_check;
    time_t ready_time;
    int primes_done;

    GstBuffer *sps_pps;        /* last codec config (frame_type 0) */
    GstBuffer *gop[GOP_MAX_BUFFERS];
    int gop_count;
    size_t gop_bytes;
} video_renderer_webos_t;

static const video_renderer_funcs_t video_renderer_webos_funcs;

static void luna_send(const char *uri, const char *payload) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "luna-send -n 1 -w 5000 %s '%s' 2>&1", uri, payload);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        printf("[webos-video] popen failed for %s\n", uri);
        return;
    }
    char result[256];
    if (fgets(result, sizeof(result), fp))
        printf("[webos-video] %s -> %s", uri, result);
    else
        printf("[webos-video] %s -> (no reply; running inside the dev-mode jail?)\n", uri);
    pclose(fp);
    fflush(stdout);
}

static void launch_app(video_renderer_webos_t *r) {
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"" APP_ID "\",\"params\":{\"udpPort\":%d,\"width\":1920,\"height\":1080,\"framerate\":30}}",
             UDP_PORT);
    luna_send("luna://com.webos.applicationManager/launch", payload);
    r->app_launched = 1;
    r->tv_ready = 0;
    r->last_ready_check = 0;
}

static int udp_port_bound(void);

static void close_app(video_renderer_webos_t *r) {
    luna_send("luna://com.webos.applicationManager/closeByAppId", "{\"id\":\"" APP_ID "\"}");
    r->app_launched = 0;
    r->tv_ready = 0;
    /* Wait for the old pipeline to release the port so the next session
     * doesn't mistake it for its own */
    for (int i = 0; i < 30 && udp_port_bound(); i++)
        usleep(100000);
}

/* The Miracast pipeline binds 0.0.0.0:UDP_PORT once the app's <video> loads. */
static int udp_port_bound(void) {
    char want[16], line[256];
    int found = 0;
    snprintf(want, sizeof(want), ":%04X ", UDP_PORT);
    FILE *f = fopen("/proc/net/udp", "r");
    if (!f) return 0;
    while (!found && fgets(line, sizeof(line), f)) {
        /* local_address is the second column: "  sl: 00000000:1388 ..." */
        char *local = strchr(line, ':');
        if (local && strstr(local + 1, want) == strchr(local + 1, ':'))
            found = 1;
    }
    fclose(f);
    return found;
}

static void gop_clear(video_renderer_webos_t *r) {
    for (int i = 0; i < r->gop_count; i++)
        gst_buffer_unref(r->gop[i]);
    r->gop_count = 0;
    r->gop_bytes = 0;
}

static void push(video_renderer_webos_t *r, GstBuffer *buffer) {
    gst_app_src_push_buffer(GST_APP_SRC(r->appsrc), buffer);
}

/* Replay codec config + GOP. The cache keeps its own buffers; pushes get copies. */
static void prime(video_renderer_webos_t *r) {
    printf("[webos-video] priming TV decoder: sps/pps=%s, %d frames (%u bytes)\n",
           r->sps_pps ? "yes" : "no", r->gop_count, (unsigned)r->gop_bytes);
    fflush(stdout);
    if (r->sps_pps) push(r, gst_buffer_copy(r->sps_pps));
    for (int i = 0; i < r->gop_count; i++)
        push(r, gst_buffer_copy(r->gop[i]));
}

static int is_idr(unsigned char *data, int data_len) {
    /* Annex B access unit: look for a NAL of type 5 */
    for (int i = 0; i + 4 < data_len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 5)
            return 1;
    }
    return 0;
}

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
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, usr1_handler);

    gst_init(NULL, NULL);

    renderer->base.logger = logger;
    renderer->base.funcs = &video_renderer_webos_funcs;
    renderer->base.type = VIDEO_RENDERER_GSTREAMER;
    pthread_mutex_init(&renderer->lock, NULL);

    char launch[512];
    snprintf(launch, sizeof(launch),
             "appsrc name=video_source is-live=true do-timestamp=true format=time stream-type=0 ! "
             "h264parse config-interval=1 ! "
             "video/x-h264,stream-format=byte-stream,alignment=au ! "
             "mpegtsmux ! rtpmp2tpay ! "
             "udpsink host=127.0.0.1 port=%d sync=false async=false",
             UDP_PORT);
    printf("[webos-video] pipeline: %s\n", launch);
    renderer->pipeline = gst_parse_launch(launch, &error);
    if (error) {
        printf("[webos-video] pipeline failed: %s\n", error->message);
        g_error_free(error);
        if (renderer->pipeline) gst_object_unref(renderer->pipeline);
        free(renderer);
        return NULL;
    }

    renderer->appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_source");
    GstCaps *caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        NULL);
    g_object_set(renderer->appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    printf("[webos-video] init complete\n");
    fflush(stdout);
    return &renderer->base;
}

static void video_renderer_webos_start(video_renderer_t *renderer) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    GstStateChangeReturn ret = gst_element_set_state(r->pipeline, GST_STATE_PLAYING);
    printf("[webos-video] PLAYING returned %d\n", ret);
    fflush(stdout);
}

static void video_renderer_webos_render_buffer(video_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts, int type) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;

    if (data_len == 0) return;

    pthread_mutex_lock(&r->lock);

    /* Covers a stream that starts without a conn_init (or after the app was closed) */
    if (!r->app_launched)
        launch_app(r);

    /* Keep codec config and the current GOP */
    GstBuffer *cached = gst_buffer_new_and_alloc(data_len);
    assert(cached != NULL);
    gst_buffer_fill(cached, 0, data, data_len);
    if (type == 0) {
        if (r->sps_pps) gst_buffer_unref(r->sps_pps);
        r->sps_pps = cached;
    } else {
        if (is_idr(data, data_len) || r->gop_count == GOP_MAX_BUFFERS ||
            r->gop_bytes + data_len > GOP_MAX_BYTES)
            gop_clear(r);
        r->gop[r->gop_count++] = cached;
        r->gop_bytes += data_len;
    }

    time_t now = time(NULL);

    if (!r->tv_ready) {
        if (now != r->last_ready_check) {
            r->last_ready_check = now;
            if (udp_port_bound()) {
                printf("[webos-video] TV pipeline listening on %d\n", UDP_PORT);
                r->tv_ready = 1;
                r->ready_time = now;
                r->primes_done = 0;
            }
        }
    } else if (prime_requested ||
               (r->primes_done < PRIME_COUNT && now - r->ready_time >= prime_delays[r->primes_done])) {
        /* The current frame is already in the GOP, so priming also sends it */
        prime_requested = 0;
        if (r->primes_done < PRIME_COUNT) r->primes_done++;
        prime(r);
        pthread_mutex_unlock(&r->lock);
        return;
    }

    if (r->tv_ready && r->primes_done > 0)
        push(r, gst_buffer_copy(cached));
    pthread_mutex_unlock(&r->lock);
}

void video_renderer_webos_flush(video_renderer_t *renderer) {
}

void video_renderer_webos_destroy(video_renderer_t *renderer) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    gst_element_set_state(r->pipeline, GST_STATE_NULL);
    gst_object_unref(r->pipeline);
    if (r->app_launched) close_app(r);
    gop_clear(r);
    if (r->sps_pps) gst_buffer_unref(r->sps_pps);
    pthread_mutex_destroy(&r->lock);
    free(renderer);
}

/* Called with +1 on connection init and -1 on connection teardown.
 * The app is shown while at least one client connection is open. */
void video_renderer_webos_update_background(video_renderer_t *renderer, int type) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    pthread_mutex_lock(&r->lock);
    if (type > 0) {
        r->connections++;
        if (!r->app_launched) launch_app(r);
    } else if (type < 0 && r->connections > 0) {
        r->connections--;
        if (r->connections == 0) {
            if (r->app_launched) close_app(r);
            gop_clear(r);
            if (r->sps_pps) {
                gst_buffer_unref(r->sps_pps);
                r->sps_pps = NULL;
            }
        }
    }
    pthread_mutex_unlock(&r->lock);
}

static const video_renderer_funcs_t video_renderer_webos_funcs = {
    .start = video_renderer_webos_start,
    .render_buffer = video_renderer_webos_render_buffer,
    .flush = video_renderer_webos_flush,
    .destroy = video_renderer_webos_destroy,
    .update_background = video_renderer_webos_update_background,
};
