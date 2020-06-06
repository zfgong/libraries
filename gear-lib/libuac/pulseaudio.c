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
#include "libuac.h"
#include <gear-lib/libthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <pulse/pulseaudio.h>

enum speaker_layout {
    SPEAKERS_UNKNOWN,     /**< Unknown setting, fallback is stereo. */
    SPEAKERS_MONO,        /**< Channels: MONO */
    SPEAKERS_STEREO,      /**< Channels: FL, FR */
    SPEAKERS_2POINT1,     /**< Channels: FL, FR, LFE */
    SPEAKERS_4POINT0,     /**< Channels: FL, FR, FC, RC */
    SPEAKERS_4POINT1,     /**< Channels: FL, FR, FC, LFE, RC */
    SPEAKERS_5POINT1,     /**< Channels: FL, FR, FC, LFE, RL, RR */
    SPEAKERS_7POINT1 = 8, /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
};

struct pulse_ctx {
    int fd;
    struct uac_ctx *parent;
    bool is_streaming;
    const char *device;

    /* pulseaudio defination */
    pa_threaded_mainloop *pa_mainloop;
    pa_context           *pa_ctx;
    pa_stream            *pa_stream;
    pa_context_state_t    pa_state;
    pa_stream_state_t     pa_stream_state;
    pa_sample_spec        pa_sample_spec;
    pa_channel_map        pa_channel_map;

    pa_server_info        pa_server_info;
    /* server info */
    pa_sample_format_t format;
    uint32_t samples_per_sec;
    uint32_t bytes_per_frame;
    uint8_t channels;
    uint64_t first_ts;
    enum speaker_layout speakers;
};

#define NSEC_PER_SEC 1000000000LL
#define NSEC_PER_MSEC 1000000L

uint64_t os_gettime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}
static inline uint64_t samples_to_ns(size_t frames, uint32_t rate)
{
    return frames * NSEC_PER_SEC / rate;
}

static inline uint64_t get_sample_time(size_t frames, uint_fast32_t rate)
{
    return os_gettime_ns() - samples_to_ns(frames, rate);
}

static pa_proplist *pulse_properties()
{
    pa_proplist *p = pa_proplist_new();

    pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, "libuac");
    pa_proplist_sets(p, PA_PROP_APPLICATION_ICON_NAME, "libuac");
    pa_proplist_sets(p, PA_PROP_MEDIA_ROLE, "production");

    return p;
}

static int pulse_context_ready(struct pulse_ctx *c)
{
    pa_threaded_mainloop_lock(c->pa_mainloop);
    if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(c->pa_ctx))) {
        pa_threaded_mainloop_unlock(c->pa_mainloop);
        return -1;
    }
    while (pa_context_get_state(c->pa_ctx) != PA_CONTEXT_READY) {
        pa_threaded_mainloop_wait(c->pa_mainloop);
    }
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    return 0;
}

static void pulse_state_cb(pa_context *pc, void *arg)
{
    struct pulse_ctx *c = arg;

    if (c->pa_ctx != pc) {
        printf("c->pa_ctx=%p, pc=%p\n", c->pa_ctx, pc);
        return;
    }

    switch (pa_context_get_state(pc)) {
        case PA_CONTEXT_UNCONNECTED:
            break;
        case PA_CONTEXT_CONNECTING:
            break;
        case PA_CONTEXT_AUTHORIZING:
            break;
        case PA_CONTEXT_SETTING_NAME:
            break;
        case PA_CONTEXT_READY:
            break;
        case PA_CONTEXT_FAILED:
            break;
        case PA_CONTEXT_TERMINATED:
            break;
        default:
            printf("%s:%d xxxx\n", __func__, __LINE__);
            break;
    }
    pa_threaded_mainloop_signal(c->pa_mainloop, 0);
}

static void server_info_cb(pa_context *pc, const pa_server_info *info, void *arg)
{
    struct pulse_ctx *c = arg;

    if (c->pa_ctx != pc) {
        printf("c->pa_ctx=%p, pc=%p\n", c->pa_ctx, pc);
        return;
    }
#if 1
    printf("========pulse audio information========\n");
    printf("      Server Version: %s\n",   info->server_version);
    printf("         Server Name: %s\n",   info->server_name);
    printf(" Default Source Name: %s\n",   info->default_source_name);
    printf("   Default Sink Name: %s\n",   info->default_sink_name);
    printf("           Host Name: %s\n",   info->host_name);
    printf("           User Name: %s\n",   info->user_name);
    printf("            Channels: %hhu\n", info->sample_spec.channels);
    printf("                Rate: %u\n",   info->sample_spec.rate);
    printf("          Frame Size: %lu\n",  pa_frame_size(&info->sample_spec));
    printf("         Sample Size: %lu\n",  pa_sample_size(&info->sample_spec));
    printf(" ChannelMap Channels: %hhu\n", info->channel_map.channels);
#endif
    memcpy(&c->pa_server_info, info, sizeof(pa_server_info));
    c->device = strdup(info->default_source_name);

    memcpy(&c->pa_channel_map, &info->channel_map, sizeof(pa_channel_map));
    //pa_channel_map_init_stereo(&c->pa_channel_map);
    pa_threaded_mainloop_signal(c->pa_mainloop, 0);
}

static void source_info_list_cb(pa_context *pc, const pa_source_info *info,
                                int eol, void *arg)
{
    struct pulse_ctx *c = arg;

    if (c->pa_ctx != pc) {
        printf("c->pa_ctx=%p, pc=%p\n", c->pa_ctx, pc);
        return;
    }
#if 0
    if (!eol) {
        printf("========pulse audio source info list========\n");
        printf("       Source Index: %u\n", info->index);
        printf("        Source Name: %s\n", info->name);
        printf(" Source Description: %s\n", info->description);
        printf("      Source Driver: %s\n", info->driver);
    }
#endif
    pa_threaded_mainloop_signal(c->pa_mainloop, 0);
}

static enum sample_format pulse_to_sample_format(pa_sample_format_t format)
{
    switch (format) {
    case PA_SAMPLE_U8:
        return SAMPLE_FORMAT_U8BIT;
    case PA_SAMPLE_S16LE:
        return SAMPLE_FORMAT_16BIT;
    case PA_SAMPLE_S32LE:
        return SAMPLE_FORMAT_32BIT;
    case PA_SAMPLE_FLOAT32LE:
        return SAMPLE_FORMAT_FLOAT;
    default:
        return SAMPLE_FORMAT_UNKNOWN;
    }
    return SAMPLE_FORMAT_UNKNOWN;
}

static enum speaker_layout pulse_channels_to_speakers(uint32_t channels)
{
    switch (channels) {
    case 1:
        return SPEAKERS_MONO;
    case 2:
        return SPEAKERS_STEREO;
    case 3:
        return SPEAKERS_2POINT1;
    case 4:
        return SPEAKERS_4POINT0;
    case 5:
        return SPEAKERS_4POINT1;
    case 6:
        return SPEAKERS_5POINT1;
    case 8:
        return SPEAKERS_7POINT1;
    }
    return SPEAKERS_UNKNOWN;
}

static void source_info_cb(pa_context *pc, const pa_source_info *info,
                                int eol, void *arg)
{
    struct pulse_ctx *c = arg;
    if (c->pa_ctx != pc) {
        printf("c->pa_ctx=%p, pc=%p\n", c->pa_ctx, pc);
        return;
    }
    if (eol != 0) {
        goto exit;
    }
    pa_sample_format_t format = info->sample_spec.format;
    if (pulse_to_sample_format(format) == SAMPLE_FORMAT_UNKNOWN) {
        format = PA_SAMPLE_FLOAT32LE;
        printf("Sample format %s not supported, using %s instead\n",
            pa_sample_format_to_string(info->sample_spec.format),
            pa_sample_format_to_string(format));
    }

    uint8_t channels = info->sample_spec.channels;
    enum speaker_layout speakers = pulse_channels_to_speakers(channels);
    if (speakers == SPEAKERS_UNKNOWN) {
        channels = 2;
        printf("%c channels not supported, using %c instead",
                        info->sample_spec.channels, channels);
    }

    c->format = format;
    c->samples_per_sec = info->sample_spec.rate;
    c->channels = channels;
    c->speakers = speakers;

exit:
    pa_threaded_mainloop_signal(c->pa_mainloop, 0);
}

static void sink_info_list_cb(pa_context *pc, const pa_sink_info *info,
                                int eol, void *arg)
{
    struct pulse_ctx *c = arg;

    if (c->pa_ctx != pc) {
        printf("c->pa_ctx=%p, pc=%p\n", c->pa_ctx, pc);
        return;
    }
#if 0
    if (!eol) {
        printf("========pulse audio sink info list========\n");
        printf("       Sink Index: %u\n", info->index);
        printf("        Sink Name: %s\n", info->name);
        printf(" Sink Description: %s\n", info->description);
        printf("      Sink Driver: %s\n", info->driver);
    }
#endif
    pa_threaded_mainloop_signal(c->pa_mainloop, 0);
}

static int pulse_get_server_info(struct pulse_ctx *c)
{
    int ret = 0;
    pa_operation *op = NULL;
    pa_operation_state_t state;

    if (pulse_context_ready(c) == -1) {
        printf("pulse_context not ready\n");
        return -1;
    }

    pa_threaded_mainloop_lock(c->pa_mainloop);
    op = pa_context_get_server_info(c->pa_ctx, server_info_cb, c);
    if (!op) {
        ret = -1;
        goto exit;
    }
    while ((state = pa_operation_get_state(op)) == PA_OPERATION_RUNNING) {
        if (state == PA_OPERATION_CANCELLED) {
            ret = -1;
            printf("%s PA_OPERATION_CANCELLED\n", __func__);
            break;
        }
        pa_threaded_mainloop_wait(c->pa_mainloop);
    }
    pa_operation_unref(op);

exit:
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    return ret;
}

static int pulse_get_sink_list(struct pulse_ctx *c)
{
    int ret = 0;
    pa_operation *op = NULL;
    pa_operation_state_t state;

    if (pulse_context_ready(c) == -1) {
        printf("pulse_context not ready\n");
        return -1;
    }

    pa_threaded_mainloop_lock(c->pa_mainloop);
    op = pa_context_get_sink_info_list(c->pa_ctx, sink_info_list_cb, c);
    if (!op) {
        ret = -1;
        goto exit;
    }
    while ((state = pa_operation_get_state(op)) == PA_OPERATION_RUNNING) {
        if (state == PA_OPERATION_CANCELLED) {
            ret = -1;
            printf("%s PA_OPERATION_CANCELLED\n", __func__);
            break;
        }
        pa_threaded_mainloop_wait(c->pa_mainloop);
    }
    pa_operation_unref(op);

exit:
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    return ret;
}

static int pulse_get_source_list(struct pulse_ctx *c)
{
    int ret = 0;
    pa_operation *op = NULL;
    pa_operation_state_t state;

    if (pulse_context_ready(c) == -1) {
        printf("pulse_context not ready\n");
        return -1;
    }

    pa_threaded_mainloop_lock(c->pa_mainloop);
    op = pa_context_get_source_info_list(c->pa_ctx, source_info_list_cb, c);
    if (!op) {
        ret = -1;
        goto exit;
    }
    while ((state = pa_operation_get_state(op)) == PA_OPERATION_RUNNING) {
        if (state == PA_OPERATION_CANCELLED) {
            ret = -1;
            printf("%s PA_OPERATION_CANCELLED\n", __func__);
            break;
        }
        pa_threaded_mainloop_wait(c->pa_mainloop);
    }
    pa_operation_unref(op);

exit:
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    return ret;
}

static int pulse_get_source_info(struct pulse_ctx *c, const char *name)
{
    int ret = 0;
    pa_operation *op = NULL;
    pa_operation_state_t state;

    if (pulse_context_ready(c) == -1) {
        printf("pulse_context not ready\n");
        return -1;
    }

    pa_threaded_mainloop_lock(c->pa_mainloop);
    op = pa_context_get_source_info_by_name(c->pa_ctx, name, source_info_cb, c);
    if (!op) {
        ret = -1;
        goto exit;
    }
    while ((state = pa_operation_get_state(op)) == PA_OPERATION_RUNNING) {
        if (state == PA_OPERATION_CANCELLED) {
            ret = -1;
            printf("%s PA_OPERATION_CANCELLED\n", __func__);
            break;
        }
        pa_threaded_mainloop_wait(c->pa_mainloop);
    }
    pa_operation_unref(op);

exit:
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    return ret;
}

static void read_cb(pa_stream *ps, size_t bytes, void *arg)
{
    struct pulse_ctx *c = arg;

    const void *frames;
    size_t nbytes;

    if (c->pa_stream != ps) {
        printf("c->pa_ctx=%p, ps=%p\n", c->pa_stream, ps);
        return;
    }
    pa_stream_peek(ps, &frames, &nbytes);
    if (!nbytes) {
            goto exit;
    } else if (!frames) {
            printf("Got audio hole of %zu bytes", nbytes);
            pa_stream_drop(ps);
            goto exit;
    }

    struct audio_frame out;
    //out.speakers = c->speakers;
    out.samples_per_sec = c->samples_per_sec;
    out.format = pulse_to_sample_format(c->format);
    out.data[0] = (uint8_t *)frames;
    out.frames = nbytes / c->bytes_per_frame;
    out.timestamp = get_sample_time(out.frames, out.samples_per_sec);
    printf("%s:%d xxxx bytes=%zu, nbytes=%zu, ts=%zu\n", __func__, __LINE__, bytes, nbytes, out.timestamp);

//    if (!data->first_ts)
//            data->first_ts = out.timestamp + STARTUP_TIMEOUT_NS;

//    if (out.timestamp > data->first_ts)
//            source_output_audio(data->source, &out);

//    data->packets++;
//    data->frames += out.frames;

    pa_stream_drop(ps);
exit:
    pa_threaded_mainloop_signal(c->pa_mainloop, 0);
}

static void write_cb(pa_stream *pa_stream, size_t bytes, void *arg)
{
    printf("%s:%d xxxx\n", __func__, __LINE__);
}

static void overflow_cb(pa_stream *pa_stream, void *arg)
{
    printf("%s:%d xxxx\n", __func__, __LINE__);
}

static void underflow_cb(pa_stream *pa_stream, void *arg)
{
    printf("%s:%d xxxx\n", __func__, __LINE__);
}

static void stream_latency_update_cb(pa_stream *s, void *arg)
{
    printf("%s:%d xxxx\n", __func__, __LINE__);
}

static void stream_state_cb(pa_stream *pa_ctx, void *arg)
{
    struct pulse_ctx *c = arg;
    if (c->pa_stream != pa_ctx) {
        printf("c->pa_ctx=%p, pa_ctx=%p\n", c->pa_stream, pa_ctx);
        return;
    }

    switch (pa_stream_get_state(pa_ctx)) {
        case PA_STREAM_CREATING:
            printf("%s:%d PA_STREAM_CREATING\n", __func__, __LINE__);
            break;
        case PA_STREAM_UNCONNECTED:
            printf("%s:%d PA_STREAM_UNCONNECTED\n", __func__, __LINE__);
            break;
        case PA_STREAM_READY:
            printf("%s:%d PA_STREAM_READY\n", __func__, __LINE__);
            break;
        case PA_STREAM_FAILED:
            printf("%s:%d PA_STREAM_FAILED %s\n", __func__, __LINE__, pa_strerror(pa_context_errno(c->pa_ctx)));
            break;
        case PA_STREAM_TERMINATED:
            printf("%s:%d PA_STREAM_UNCONNECTED\n", __func__, __LINE__);
            break;
    }
    pa_threaded_mainloop_signal(c->pa_mainloop, 0);
}

static int pulse_channel_map(pa_channel_map *pm, enum speaker_layout speakers)
{
    pm->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
    pm->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
    pm->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
    pm->map[3] = PA_CHANNEL_POSITION_LFE;
    pm->map[4] = PA_CHANNEL_POSITION_REAR_LEFT;
    pm->map[5] = PA_CHANNEL_POSITION_REAR_RIGHT;
    pm->map[6] = PA_CHANNEL_POSITION_SIDE_LEFT;
    pm->map[7] = PA_CHANNEL_POSITION_SIDE_RIGHT;

    switch (speakers) {
    case SPEAKERS_MONO:
        pm->channels = 1;
        pm->map[0] = PA_CHANNEL_POSITION_MONO;
        break;
    case SPEAKERS_STEREO:
        pm->channels = 2;
        break;
    case SPEAKERS_2POINT1:
        pm->channels = 3;
        pm->map[2] = PA_CHANNEL_POSITION_LFE;
        break;
    case SPEAKERS_4POINT0:
        pm->channels = 4;
        pm->map[3] = PA_CHANNEL_POSITION_REAR_CENTER;
        break;
    case SPEAKERS_4POINT1:
        pm->channels = 5;
        pm->map[4] = PA_CHANNEL_POSITION_REAR_CENTER;
        break;
    case SPEAKERS_5POINT1:
        pm->channels = 6;
        break;
    case SPEAKERS_7POINT1:
        pm->channels = 8;
        break;
    case SPEAKERS_UNKNOWN:
    default:
        pm->channels = 0;
        break;
    }
    return 0;
}


static void *uac_pa_open(struct uac_ctx *uac, const char *dev, struct uac_config *conf)
{
    struct pulse_ctx *c = calloc(1, sizeof(struct pulse_ctx));

    c->pa_mainloop = pa_threaded_mainloop_new();
    if (!c->pa_mainloop) {
        printf("pa_threaded_mainloop_new failed!\n");
    }
    pa_proplist *p = pulse_properties();
    c->pa_ctx = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(c->pa_mainloop), "libuac", p);
    if (!c->pa_ctx) {
        printf("pa_context_new failed!\n");
    }

    pa_context_set_state_callback(c->pa_ctx, pulse_state_cb, c);
    pa_context_connect(c->pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
    pa_threaded_mainloop_start(c->pa_mainloop);

    pa_threaded_mainloop_lock(c->pa_mainloop);
    while ((c->pa_state = pa_context_get_state(c->pa_ctx)) != PA_CONTEXT_READY) {
        if (c->pa_state == PA_CONTEXT_FAILED || c->pa_state == PA_CONTEXT_TERMINATED) {
            printf("pa_context_state is invalid!\n");
            break;
        }
        pa_threaded_mainloop_wait(c->pa_mainloop);
    }
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    //pa_context_set_state_callback(c->pa_ctx, NULL, c);

    if (c->pa_state != PA_CONTEXT_READY) {
        printf("pa_context_state is not ready!\n");
        goto failed;
    }

    pulse_get_server_info(c);
    pulse_get_sink_list(c);
    pulse_get_source_list(c);

    c->parent = uac;

    return c;

failed:

    pa_threaded_mainloop_stop(c->pa_mainloop);
    free(c);
    return NULL;
}

static int uac_pa_start_stream(struct uac_ctx *uac)
{
    struct pulse_ctx *c = (struct pulse_ctx *)uac->opaque;
    int ret;
    pa_buffer_attr attr = { -1 };

    pulse_get_source_info(c, c->pa_server_info.default_source_name);

    c->pa_sample_spec.format = c->format;
    c->pa_sample_spec.rate  = c->samples_per_sec;
    c->pa_sample_spec.channels = c->channels;

    if (!pa_sample_spec_valid(&c->pa_sample_spec)) {
        printf("Sample spec is not valid\n");
        return -1;
    }

    c->bytes_per_frame = pa_frame_size(&c->pa_sample_spec);

    pulse_channel_map(&c->pa_channel_map, c->speakers);
    printf("pa_stream_new %s\n", c->device);
    pa_proplist *p = pulse_properties();

    c->pa_stream = pa_stream_new_with_proplist(c->pa_ctx, c->device, &c->pa_sample_spec, &c->pa_channel_map, p);
    if (!c->pa_stream) {
        printf("pa_stream_new failed!\n");
    }

    printf("c=%p\n", c);
    pa_threaded_mainloop_lock(c->pa_mainloop);
    pa_stream_set_read_callback(c->pa_stream, read_cb, c);
    pa_stream_set_write_callback(c->pa_stream, write_cb, c);
    pa_stream_set_state_callback(c->pa_stream, stream_state_cb, c);
    pa_stream_set_overflow_callback(c->pa_stream, overflow_cb, c);
    pa_stream_set_underflow_callback(c->pa_stream, underflow_cb, c);
    pa_stream_set_latency_update_callback(c->pa_stream, stream_latency_update_cb, c);
    pa_threaded_mainloop_unlock(c->pa_mainloop);

    attr.fragsize = pa_usec_to_bytes(25000, &c->pa_sample_spec);
    attr.maxlength = (uint32_t)-1;
    attr.minreq = (uint32_t)-1;
    attr.prebuf = (uint32_t)-1;
    attr.tlength = (uint32_t)-1;

    printf("gggxxx %s\n", pa_context_get_server(c->pa_ctx));
    pa_stream_flags_t flags = PA_STREAM_INTERPOLATE_TIMING
                            | PA_STREAM_ADJUST_LATENCY
                            | PA_STREAM_AUTO_TIMING_UPDATE;

    pa_threaded_mainloop_lock(c->pa_mainloop);
    ret = pa_stream_connect_record(c->pa_stream, c->device, &attr, flags);
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    if (ret < 0) {
        printf("pa_stream_connect_record failed %d", pa_context_errno(c->pa_ctx));
    }

    pa_threaded_mainloop_lock(c->pa_mainloop);
    while ((c->pa_stream_state = pa_stream_get_state(c->pa_stream)) != PA_STREAM_READY) {
        if (c->pa_stream_state == PA_STREAM_FAILED ||
            c->pa_stream_state == PA_STREAM_TERMINATED) {
            printf("%s:%d xxxx\n", __func__, __LINE__);
            break;
        }
            printf("%s:%d xxxx\n", __func__, __LINE__);
        pa_threaded_mainloop_wait(c->pa_mainloop);
    }
    pa_threaded_mainloop_unlock(c->pa_mainloop);
    return 0;
}

static int uac_pa_stop_stream(struct uac_ctx *uac)
{
   return 0;
}

static int uac_pa_query_frame(struct uac_ctx *uac, struct audio_frame *frame)
{
    return 0;
}

static void uac_pa_close(struct uac_ctx *uac)
{
}

struct uac_ops pa_ops = {
    .open         = uac_pa_open,
    .close        = uac_pa_close,
    .ioctl        = NULL,
    .start_stream = uac_pa_start_stream,
    .stop_stream  = uac_pa_stop_stream,
    .query_frame  = uac_pa_query_frame,
};