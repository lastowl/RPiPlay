/*
 * Audio renderer for LG webOS TVs
 *
 * AirPlay mirroring audio (AAC-ELD, 44.1 kHz stereo, 480-sample frames) is
 * decoded with fdk-aac, re-encoded as AAC-LC/ADTS and handed to the video
 * renderer, which muxes it into the MPEG-TS stream the TV's Miracast
 * pipeline plays. The TV then owns audio output and A/V sync.
 *
 * mpegtsmux waits for data on every input, so a pacing thread emits one
 * AAC frame per 1024 samples of wall-clock time, filling with silence when
 * the phone sends nothing.
 */

#include "audio_renderer.h"
#include "webos_av.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fdk-aac/libAACdec/include/aacdecoder_lib.h"
#include "fdk-aac/libAACenc/include/aacenc_lib.h"

#define ELD_FRAME_SAMPLES 480
#define LC_FRAME_SAMPLES 1024
#define RING_FRAMES (WEBOS_AUDIO_RATE / 2)      /* 0.5 s of stereo frames */
#define PREBUFFER_FRAMES 2048                    /* ~46 ms before playing after a gap */
#define MAX_LATENCY_FRAMES (WEBOS_AUDIO_RATE / 4) /* drop oldest beyond ~250 ms */

typedef struct audio_renderer_webos_s {
    audio_renderer_t base;
    video_renderer_t *video_renderer;
    HANDLE_AACDECODER decoder;
    HANDLE_AACENCODER encoder;

    pthread_t thread;
    int running;
    pthread_mutex_t lock;

    /* Ring of interleaved stereo int16 frames */
    INT_PCM ring[RING_FRAMES * WEBOS_AUDIO_CHANNELS];
    int ring_read, ring_fill;
    int starved;
} audio_renderer_webos_t;

static const audio_renderer_funcs_t audio_renderer_webos_funcs;

static int open_decoder(audio_renderer_webos_t *r) {
    r->decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!r->decoder) return -1;
    /* AudioSpecificConfig for AAC-ELD 44.1 kHz stereo, 480 samples/frame */
    UCHAR eld_conf[] = { 0xF8, 0xE8, 0x50, 0x00 };
    UCHAR *conf[] = { eld_conf };
    UINT conf_len = sizeof(eld_conf);
    if (aacDecoder_ConfigRaw(r->decoder, conf, &conf_len) != AAC_DEC_OK) return -2;
    return 0;
}

static int open_encoder(audio_renderer_webos_t *r) {
    if (aacEncOpen(&r->encoder, 0, WEBOS_AUDIO_CHANNELS) != AACENC_OK) return -1;
    if (aacEncoder_SetParam(r->encoder, AACENC_AOT, AOT_AAC_LC) != AACENC_OK ||
        aacEncoder_SetParam(r->encoder, AACENC_SAMPLERATE, WEBOS_AUDIO_RATE) != AACENC_OK ||
        aacEncoder_SetParam(r->encoder, AACENC_CHANNELMODE, MODE_2) != AACENC_OK ||
        aacEncoder_SetParam(r->encoder, AACENC_BITRATE, 192000) != AACENC_OK ||
        aacEncoder_SetParam(r->encoder, AACENC_TRANSMUX, TT_MP4_ADTS) != AACENC_OK)
        return -2;
    if (aacEncEncode(r->encoder, NULL, NULL, NULL, NULL) != AACENC_OK) return -3;
    return 0;
}

/* Take one LC frame of PCM from the ring, zero-filling on underrun. */
static void take_frame(audio_renderer_webos_t *r, INT_PCM *out) {
    pthread_mutex_lock(&r->lock);
    int want = LC_FRAME_SAMPLES;
    if (r->starved && r->ring_fill >= PREBUFFER_FRAMES)
        r->starved = 0;
    int avail = r->starved ? 0 : (r->ring_fill < want ? r->ring_fill : want);
    for (int i = 0; i < avail; i++) {
        int idx = ((r->ring_read + i) % RING_FRAMES) * WEBOS_AUDIO_CHANNELS;
        for (int c = 0; c < WEBOS_AUDIO_CHANNELS; c++)
            out[i * WEBOS_AUDIO_CHANNELS + c] = r->ring[idx + c];
    }
    r->ring_read = (r->ring_read + avail) % RING_FRAMES;
    r->ring_fill -= avail;
    if (avail < want) {
        memset(out + avail * WEBOS_AUDIO_CHANNELS, 0,
               (want - avail) * WEBOS_AUDIO_CHANNELS * sizeof(INT_PCM));
        r->starved = 1;
    }
    pthread_mutex_unlock(&r->lock);
}

static void encode_and_push(audio_renderer_webos_t *r, INT_PCM *pcm) {
    UCHAR out[4096];
    void *in_ptr = pcm, *out_ptr = out;
    INT in_id = IN_AUDIO_DATA, out_id = OUT_BITSTREAM_DATA;
    INT in_size = LC_FRAME_SAMPLES * WEBOS_AUDIO_CHANNELS * sizeof(INT_PCM), out_size = sizeof(out);
    INT in_el = sizeof(INT_PCM), out_el = 1;
    AACENC_BufDesc in_buf = { 1, &in_ptr, &in_id, &in_size, &in_el };
    AACENC_BufDesc out_buf = { 1, &out_ptr, &out_id, &out_size, &out_el };
    AACENC_InArgs in_args = { LC_FRAME_SAMPLES * WEBOS_AUDIO_CHANNELS, 0 };
    AACENC_OutArgs out_args;
    memset(&out_args, 0, sizeof(out_args));

    if (aacEncEncode(r->encoder, &in_buf, &out_buf, &in_args, &out_args) != AACENC_OK)
        return;
    if (out_args.numOutBytes > 0)
        video_renderer_webos_push_audio(r->video_renderer, out, out_args.numOutBytes);
}

static void timespec_add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static void *pacing_thread(void *arg) {
    audio_renderer_webos_t *r = arg;
    INT_PCM pcm[LC_FRAME_SAMPLES * WEBOS_AUDIO_CHANNELS];
    struct timespec next;
    /* 1024 / 44100 s, with the remainder carried so the rate doesn't drift */
    const long long frame_ns_num = 1000000000LL * LC_FRAME_SAMPLES;
    long long carry = 0;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (r->running) {
        take_frame(r, pcm);
        encode_and_push(r, pcm);

        carry += frame_ns_num;
        long step = (long)(carry / WEBOS_AUDIO_RATE);
        carry -= (long long)step * WEBOS_AUDIO_RATE;
        timespec_add_ns(&next, step);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

audio_renderer_t *audio_renderer_gstreamer_init(logger_t *logger, video_renderer_t *video_renderer, audio_renderer_config_t const *config) {
    if (!video_renderer || video_renderer->type != VIDEO_RENDERER_GSTREAMER) {
        printf("[webos-audio] needs the webOS video renderer to mux into\n");
        return NULL;
    }

    audio_renderer_webos_t *r = calloc(1, sizeof(audio_renderer_webos_t));
    if (!r) return NULL;
    r->base.logger = logger;
    r->base.funcs = &audio_renderer_webos_funcs;
    r->base.type = AUDIO_RENDERER_GSTREAMER;
    r->video_renderer = video_renderer;
    r->starved = 1;
    pthread_mutex_init(&r->lock, NULL);

    int ret;
    if ((ret = open_decoder(r)) != 0) {
        printf("[webos-audio] AAC-ELD decoder init failed (%d)\n", ret);
        free(r);
        return NULL;
    }
    if ((ret = open_encoder(r)) != 0) {
        printf("[webos-audio] AAC-LC encoder init failed (%d)\n", ret);
        aacDecoder_Close(r->decoder);
        free(r);
        return NULL;
    }
    printf("[webos-audio] init complete (AAC-ELD -> AAC-LC %d Hz, muxed with video)\n", WEBOS_AUDIO_RATE);
    fflush(stdout);
    return &r->base;
}

static void audio_renderer_webos_start(audio_renderer_t *renderer) {
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;
    r->running = 1;
    if (pthread_create(&r->thread, NULL, pacing_thread, r) != 0) {
        printf("[webos-audio] failed to start pacing thread\n");
        r->running = 0;
    }
}

static void audio_renderer_webos_render_buffer(audio_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts) {
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;
    if (data_len == 0) return;

    UCHAR *p_buffer[1] = { data };
    UINT buffer_size = data_len, bytes_valid = data_len;
    if (aacDecoder_Fill(r->decoder, p_buffer, &buffer_size, &bytes_valid) != AAC_DEC_OK)
        return;

    INT_PCM pcm[ELD_FRAME_SAMPLES * WEBOS_AUDIO_CHANNELS];
    if (aacDecoder_DecodeFrame(r->decoder, pcm, sizeof(pcm) / sizeof(INT_PCM), 0) != AAC_DEC_OK)
        return;

    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < ELD_FRAME_SAMPLES; i++) {
        if (r->ring_fill == RING_FRAMES) {
            /* Full: drop the oldest frame */
            r->ring_read = (r->ring_read + 1) % RING_FRAMES;
            r->ring_fill--;
        }
        int idx = ((r->ring_read + r->ring_fill) % RING_FRAMES) * WEBOS_AUDIO_CHANNELS;
        for (int c = 0; c < WEBOS_AUDIO_CHANNELS; c++)
            r->ring[idx + c] = pcm[i * WEBOS_AUDIO_CHANNELS + c];
        r->ring_fill++;
    }
    /* Keep latency bounded if the sender's clock runs fast */
    if (r->ring_fill > MAX_LATENCY_FRAMES) {
        int drop = r->ring_fill - PREBUFFER_FRAMES;
        r->ring_read = (r->ring_read + drop) % RING_FRAMES;
        r->ring_fill -= drop;
    }
    pthread_mutex_unlock(&r->lock);
}

/*
 * The iPhone's volume slider (16 steps of 30/16 dB above mute) drives the
 * TV's master volume (0-100) along tv_volume_curve: fine steps at the bottom
 * and coarse ones at the top, so low volumes stay adjustable and full volume
 * is reachable. Each step moves from the TV's current volume, so the TV
 * remote keeps working alongside. At connect iOS asks for the current volume
 * (get_volume); the answer puts the slider at the TV's place on the curve.
 */
#define IOS_VOLUME_STEPS 16
#define IOS_VOLUME_STEP_DB (30.0f / IOS_VOLUME_STEPS)
#define IOS_MIN_DB -30.0f
#define IOS_MUTE_DB -144.0f

static const int tv_volume_curve[IOS_VOLUME_STEPS + 1] = {
    0, 1, 2, 4, 6, 8, 10, 13, 17, 22, 28, 35, 43, 54, 66, 82, 100
};

/* The point on the curve nearest to a TV volume */
static int curve_position(int tv_volume) {
    int best = 0;
    for (int i = 1; i <= IOS_VOLUME_STEPS; i++)
        if (abs(tv_volume_curve[i] - tv_volume) < abs(tv_volume_curve[best] - tv_volume))
            best = i;
    return best;
}

static float volume_ref;        /* iOS volume that matches the TV's current volume */
static int volume_ref_set, volume_muted;

void audio_renderer_webos_session_end(void) {
    volume_ref_set = 0;
    volume_muted = 0;
}

/* Runs luna-send; returns the first line of the reply in out. */
static int luna_call(const char *uri, const char *payload, char *out, int out_len) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "luna-send -n 1 -w 3000 %s '%s' 2>/dev/null", uri, payload);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    int ok = fgets(out, out_len, fp) != NULL;
    pclose(fp);
    return ok;
}

static int tv_get_volume(void) {
    char reply[256];
    const char *v;
    if (!luna_call("luna://com.webos.audio/getVolume", "{}", reply, sizeof(reply)) ||
        !(v = strstr(reply, "\"volume\":")))
        return -1;
    return atoi(v + 9);
}

static void tv_set_muted(int muted) {
    char reply[256];
    luna_call("luna://com.webos.audio/setMuted", muted ? "{\"muted\":true}" : "{\"muted\":false}",
              reply, sizeof(reply));
    printf("[webos-audio] TV %s\n", muted ? "muted" : "unmuted");
    fflush(stdout);
}

static float audio_renderer_webos_get_volume(audio_renderer_t *renderer) {
    (void)renderer;
    int tv = tv_get_volume();
    int position = tv < 0 ? IOS_VOLUME_STEPS / 2 : curve_position(tv);
    volume_ref = IOS_MIN_DB + position * IOS_VOLUME_STEP_DB;
    volume_ref_set = 1;
    volume_muted = 0;
    printf("[webos-audio] TV volume %d: reporting %.2f dB to iOS\n", tv, volume_ref);
    fflush(stdout);
    return volume_ref;
}

/* AirPlay volume: 0 dB (max) down to -30 dB, -144 for mute */
static void audio_renderer_webos_set_volume(audio_renderer_t *renderer, float volume) {
    (void)renderer;
    if (volume <= IOS_MUTE_DB + 1.0f) {
        if (!volume_muted) tv_set_muted(1);
        volume_muted = 1;
        return;
    }
    if (volume_muted) {
        tv_set_muted(0);        /* then step from the volume before muting */
        volume_muted = 0;
    }
    if (!volume_ref_set) {
        volume_ref = volume;
        volume_ref_set = 1;
        printf("[webos-audio] iOS volume %.1f dB (not queried at connect), TV volume left as is\n", volume);
        fflush(stdout);
        return;
    }
    int steps = (int)roundf((volume - volume_ref) / IOS_VOLUME_STEP_DB);
    if (steps == 0) return;
    volume_ref += steps * IOS_VOLUME_STEP_DB;
    int current = tv_get_volume();
    if (current < 0) return;
    int from = curve_position(current);
    int position = from + steps;
    /* Step down no further than 1 and leave silence to mute, so pushing the
       slider to either end lines it up with the TV again */
    if (position < 1) position = from < 1 ? from : 1;
    if (position > IOS_VOLUME_STEPS) position = IOS_VOLUME_STEPS;
    int target = tv_volume_curve[position];
    char payload[64], reply[256];
    snprintf(payload, sizeof(payload), "{\"volume\":%d}", target);
    luna_call("luna://com.webos.audio/setVolume", payload, reply, sizeof(reply));
    printf("[webos-audio] iOS volume %.1f dB: TV volume %d -> %d\n", volume, current, target);
    fflush(stdout);
}

static void audio_renderer_webos_flush(audio_renderer_t *renderer) {
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;
    pthread_mutex_lock(&r->lock);
    r->ring_read = 0;
    r->ring_fill = 0;
    r->starved = 1;
    pthread_mutex_unlock(&r->lock);
}

static void audio_renderer_webos_destroy(audio_renderer_t *renderer) {
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;
    if (r->running) {
        r->running = 0;
        pthread_join(r->thread, NULL);
    }
    aacEncClose(&r->encoder);
    aacDecoder_Close(r->decoder);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static const audio_renderer_funcs_t audio_renderer_webos_funcs = {
    .start = audio_renderer_webos_start,
    .render_buffer = audio_renderer_webos_render_buffer,
    .set_volume = audio_renderer_webos_set_volume,
    .get_volume = audio_renderer_webos_get_volume,
    .flush = audio_renderer_webos_flush,
    .destroy = audio_renderer_webos_destroy,
};
