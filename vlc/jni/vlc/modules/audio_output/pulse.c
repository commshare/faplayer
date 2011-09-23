/*****************************************************************************
 * pulse.c : Pulseaudio output plugin for vlc
 *****************************************************************************
 * Copyright (C) 2008 the VideoLAN team
 * Copyright (C) 2009-2011 Rémi Denis-Courmont
 *
 * Authors: Martin Hamrle <hamrle @ post . cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <math.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_cpu.h>

#include <pulse/pulseaudio.h>
#include <vlc_pulse.h>
#if !PA_CHECK_VERSION(0,9,22)
# include <vlc_xlib.h>
#endif

static int  Open        ( vlc_object_t * );
static void Close       ( vlc_object_t * );

vlc_module_begin ()
    set_shortname( "PulseAudio" )
    set_description( N_("Pulseaudio audio output") )
    set_capability( "audio output", 160 )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_AOUT )
    add_shortcut( "pulseaudio", "pa" )
    set_callbacks( Open, Close )
vlc_module_end ()

/* TODO:
 * - pause input on policy event
 * - resample to compensate for long term drift
 * - select music or video stream property correctly (?)
 * - set further appropriate stream properties
 * - update output devices list dynamically
 */

/* NOTE:
 * Be careful what you do when the PulseAudio mainloop is held, which is to say
 * within PulseAudio callbacks, or after vlc_pa_lock().
 * In particular, a VLC variable callback cannot be triggered nor deleted with
 * the PulseAudio mainloop lock held, if the callback acquires the lock. */

struct aout_sys_t
{
    pa_stream *stream; /**< PulseAudio playback stream object */
    pa_context *context; /**< PulseAudio connection context */
    pa_volume_t base_volume; /**< 0dB reference volume */
    pa_cvolume cvolume; /**< actual sink input volume */
    mtime_t paused; /**< Time when (last) paused */
    mtime_t pts; /**< Play time of buffer write offset */
    mtime_t desync; /**< Measured desynchronization */
    unsigned rate; /**< Current stream sample rate */
};

static void sink_input_info_cb(pa_context *, const pa_sink_input_info *,
                               int, void *);

/*** Context ***/
static void context_cb(pa_context *ctx, pa_subscription_event_type_t type,
                       uint32_t idx, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;
    pa_operation *op;

    switch (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
    {
      case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
        if (idx != pa_stream_get_index(sys->stream))
            break; /* only interested in our sink input */

        /* Gee... PA will not provide the infos directly in the event. */
        switch (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK)
        {
          case PA_SUBSCRIPTION_EVENT_REMOVE:
            msg_Err(aout, "sink input killed!");
            break;

          default:
            op = pa_context_get_sink_input_info(ctx, idx, sink_input_info_cb,
                                                aout);
            if (likely(op != NULL))
                pa_operation_unref(op);
            break;
        }
        break;

      default: /* unsubscribed facility?! */
        assert(0);
    }
}


/*** Sink ***/
static void sink_list_cb(pa_context *c, const pa_sink_info *i, int eol,
                         void *userdata)
{
    audio_output_t *aout = userdata;
    vlc_value_t val, text;

    if (eol)
        return;
    (void) c;

    msg_Dbg(aout, "listing sink %s (%"PRIu32"): %s", i->name, i->index,
            i->description);
    val.i_int = i->index;
    text.psz_string = (char *)i->description;
    var_Change(aout, "audio-device", VLC_VAR_ADDCHOICE, &val, &text);
}

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol,
                         void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;

    if (eol)
        return;
    (void) c;

    /* PulseAudio flat volume NORM / 100% / 0dB corresponds to no software
     * amplification and maximum hardware amplification.
     * VLC maps DEFAULT / 100% to no gain at all (software/hardware).
     * Thus we need to use the sink base_volume as a multiplier,
     * if and only if flat volume is active for our current sink. */
    if (i->flags & PA_SINK_FLAT_VOLUME)
        sys->base_volume = i->base_volume;
    else
        sys->base_volume = PA_VOLUME_NORM;
    msg_Dbg(aout, "base volume: %"PRIu32, sys->base_volume);
}


/*** Latency management and lip synchronization ***/
static mtime_t vlc_pa_get_latency(audio_output_t *aout,
                                  pa_context *ctx, pa_stream *s)
{
    pa_usec_t latency;
    int negative;

    if (pa_stream_get_latency(s, &latency, &negative)) {
        if (pa_context_errno (ctx) != PA_ERR_NODATA)
            vlc_pa_error(aout, "unknown latency", ctx);
        return VLC_TS_INVALID;
    }
    return negative ? -latency : +latency;
}

static void stream_reset_sync(pa_stream *s, audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;
    const unsigned rate = aout->format.i_rate;

    sys->pts = VLC_TS_INVALID;
    sys->desync = 0;
    pa_operation *op = pa_stream_update_sample_rate(s, rate, NULL, NULL);
    if (unlikely(op == NULL))
        return;
    pa_operation_unref(op);
    sys->rate = rate;
}

/**
 * Starts or resumes the playback stream.
 * Tries start playing back audio samples at the most accurate time
 * in order to minimize desync and resampling during early playback.
 * @note PulseAudio lock required.
 */
static void stream_resync(audio_output_t *aout, pa_stream *s)
{
    aout_sys_t *sys = aout->sys;
    pa_operation *op;
    mtime_t delta;

    assert (pa_stream_is_corked(s) > 0);
    assert (sys->pts != VLC_TS_INVALID);

    delta = vlc_pa_get_latency(aout, sys->context, s);
    if (unlikely(delta == VLC_TS_INVALID))
        delta = 0; /* screwed */

    delta = (sys->pts - mdate()) - delta;

    /* TODO: adjust prebuf instead of padding? */
    if (delta > 0) {
        size_t nb = (delta * sys->rate) / CLOCK_FREQ;
        size_t size = aout->format.i_bytes_per_frame;
        float *zeroes = calloc (nb, size);

        msg_Dbg(aout, "starting with %zu zeroes (%"PRId64" us)", nb,
                delta);
#if 0 /* Fault injector: add delay */
        pa_stream_write(s, zeroes, nb * size, NULL, 0, PA_SEEK_RELATIVE);
        pa_stream_write(s, zeroes, nb * size, NULL, 0, PA_SEEK_RELATIVE);
#endif
        if (likely(zeroes != NULL))
            if (pa_stream_write(s, zeroes, nb * size, free, 0,
                                PA_SEEK_RELATIVE) < 0)
                free(zeroes);
    } else
        msg_Warn(aout, "starting late (%"PRId64" us)", delta);

    op = pa_stream_cork(s, 0, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
    op = pa_stream_trigger(s, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
}

static void stream_latency_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;
    mtime_t delta, change;

    if (pa_stream_is_corked(s))
        return;
    if (sys->pts == VLC_TS_INVALID)
    {
        msg_Dbg(aout, "missing latency from input");
        return;
    }

    /* Compute lip desynchronization */
    delta = vlc_pa_get_latency(aout, sys->context, s);
    if (delta == VLC_TS_INVALID)
        return;

    delta = (sys->pts - mdate()) - delta;
    change = delta - sys->desync;
    sys->desync = delta;
    //msg_Dbg(aout, "desync: %+"PRId64" us (variation: %+"PRId64" us)",
    //        delta, change);

    const unsigned inrate = aout->format.i_rate;
    unsigned outrate = sys->rate;
    bool sync = false;

    if (delta < -AOUT_MAX_PTS_DELAY)
        msg_Warn(aout, "too late by %"PRId64" us", -delta);
    else if (delta > +AOUT_MAX_PTS_ADVANCE)
        msg_Warn(aout, "too early by %"PRId64" us", delta);
    else if (outrate  == inrate)
        return; /* In sync, do not add unnecessary disturbance! */
    else
        sync = true;

    /* Compute playback sample rate */
    /* This is empirical (especially the shift values).
     * Feel free to define something smarter. */
    int adj = sync ? (outrate - inrate)
                   : outrate * ((delta >> 4) + change) / (CLOCK_FREQ << 2);
    /* This avoids too quick rate variation. It sounds really bad and
     * causes unstability (e.g. oscillation around the correct rate). */
    int limit = inrate >> 10;
    /* However, to improve stability and try to converge, closing to the
     * nominal rate is favored over drifting from it. */
    if ((adj > 0) == (sys->rate > inrate))
        limit *= 2;
    if (adj > +limit)
        adj = +limit;
    if (adj < -limit)
        adj = -limit;
    outrate -= adj;

    /* This keeps the effective rate within specified range
     * (+/-AOUT_MAX_RESAMPLING% - see <vlc_aout.h>) of the nominal rate. */
    limit = inrate * AOUT_MAX_RESAMPLING / 100;
    if (outrate > inrate + limit)
        outrate = inrate + limit;
    if (outrate < inrate - limit)
        outrate = inrate - limit;

    /* Apply adjusted sample rate */
    if (outrate == sys->rate)
        return;
    pa_operation *op = pa_stream_update_sample_rate(s, outrate, NULL, NULL);
    if (unlikely(op == NULL)) {
        vlc_pa_error(aout, "cannot change sample rate", sys->context);
        return;
    }
    pa_operation_unref(op);
    msg_Dbg(aout, "changed sample rate to %u Hz",outrate);
    sys->rate = outrate;
}


/*** Stream helpers ***/
static void stream_state_cb(pa_stream *s, void *userdata)
{
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            vlc_pa_signal(0);
        default:
            break;
    }
    (void) userdata;
}

static void stream_moved_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;
    pa_operation *op;
    uint32_t idx = pa_stream_get_device_index(s);

    msg_Dbg(aout, "connected to sink %"PRIu32": %s", idx,
                  pa_stream_get_device_name(s));
    op = pa_context_get_sink_info_by_index(sys->context, idx,
                                           sink_info_cb, aout);
    if (likely(op != NULL))
        pa_operation_unref(op);

    /* Update the variable if someone else moved our stream */
    var_Change(aout, "audio-device", VLC_VAR_SETVALUE,
               &(vlc_value_t){ .i_int = idx }, NULL);
}

static void stream_overflow_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;

    msg_Err(aout, "overflow");
    (void) s;
}

static void stream_started_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;

    msg_Dbg(aout, "started");
    (void) s;
}

static void stream_suspended_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;

    msg_Dbg(aout, "suspended");
    stream_reset_sync(s, aout);
}

static void stream_underflow_cb(pa_stream *s, void *userdata)
{
    audio_output_t *aout = userdata;
    pa_operation *op;

    msg_Warn(aout, "underflow");
    op = pa_stream_cork(s, 1, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
    stream_reset_sync(s, aout);
}

static int stream_wait(pa_stream *stream)
{
    pa_stream_state_t state;

    while ((state = pa_stream_get_state(stream)) != PA_STREAM_READY) {
        if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
            return -1;
        vlc_pa_wait();
    }
    return 0;
}


/*** Sink input ***/
static void sink_input_info_cb(pa_context *ctx, const pa_sink_input_info *i,
                               int eol, void *userdata)
{
    audio_output_t *aout = userdata;
    aout_sys_t *sys = aout->sys;
    float volume;

    if (eol)
        return;
    (void) ctx;

    sys->cvolume = i->volume;
    volume = pa_cvolume_max(&i->volume) / (float)PA_VOLUME_NORM;
    aout_VolumeHardSet(aout, volume, i->mute);
}


/*** VLC audio output callbacks ***/

/* Memory free callback. The block_t address is in front of the data. */
static void data_free(void *data)
{
    block_t **pp = data, *block;

    memcpy(&block, pp - 1, sizeof (block));
    block_Release(block);
}

static void *data_convert(block_t **pp)
{
    block_t *block = *pp;
    /* In most cases, there is enough head room, and this is really cheap: */
    block = block_Realloc(block, sizeof (block), block->i_buffer);
    *pp = block;
    if (unlikely(block == NULL))
        return NULL;

    memcpy(block->p_buffer, &block, sizeof (block));
    block->p_buffer += sizeof (block);
    block->i_buffer -= sizeof (block);
    return block->p_buffer;
}

/**
 * Queue one audio frame to the playabck stream
 */
static void Play(audio_output_t *aout, block_t *block)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;

    const void *ptr = data_convert(&block);
    if (unlikely(ptr == NULL))
        return;

    size_t len = block->i_buffer;
    mtime_t pts = block->i_pts + block->i_length;

    /* Note: The core already holds the output FIFO lock at this point.
     * Therefore we must not under any circumstances (try to) acquire the
     * output FIFO lock while the PulseAudio threaded main loop lock is held
     * (including from PulseAudio stream callbacks). Otherwise lock inversion
     * will take place, and sooner or later a deadlock. */
    vlc_pa_lock();

    sys->pts = pts;
    if (pa_stream_is_corked(s) > 0)
        stream_resync(aout, s);

#if 0 /* Fault injector to test underrun recovery */
    static volatile unsigned u = 0;
    if ((++u % 1000) == 0) {
        msg_Err(aout, "fault injection");
        pa_operation_unref(pa_stream_flush(s, NULL, NULL));
    }
#endif

    if (pa_stream_write(s, ptr, len, data_free, 0, PA_SEEK_RELATIVE) < 0) {
        vlc_pa_error(aout, "cannot write", sys->context);
        block_Release(block);
    }

    vlc_pa_unlock();
}

/**
 * Cork or uncork the playback stream
 */
static void Pause(audio_output_t *aout, bool paused, mtime_t date)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;
    pa_operation *op;

    vlc_pa_lock();

    if (paused) {
        sys->paused = date;
        op = pa_stream_cork(s, paused, NULL, NULL);
        if (op != NULL)
            pa_operation_unref(op);
    } else {
        assert (sys->paused != VLC_TS_INVALID);
        date -= sys->paused;
        msg_Dbg(aout, "resuming after %"PRId64" us", date);
        sys->paused = VLC_TS_INVALID;
        sys->pts += date;
        stream_resync(aout, s);
    }

    vlc_pa_unlock();
}

/**
 * Flush or drain the playback stream
 */
static void Flush(audio_output_t *aout, bool wait)
{
    aout_sys_t *sys = aout->sys;
    pa_stream *s = sys->stream;
    pa_operation *op;

    vlc_pa_lock();

    if (wait)
        op = pa_stream_drain(s, NULL, NULL);
        /* TODO: wait for drain completion*/
    else
        op = pa_stream_flush(s, NULL, NULL);
    if (op != NULL)
        pa_operation_unref(op);
    vlc_pa_unlock();
}

static int VolumeSet(audio_output_t *aout, float vol, bool mute)
{
    aout_sys_t *sys = aout->sys;
    pa_operation *op;
    uint32_t idx = pa_stream_get_index(sys->stream);

    pa_cvolume cvolume = sys->cvolume;
    pa_volume_t volume = sys->base_volume;

    pa_cvolume_scale(&cvolume, PA_VOLUME_NORM); /* preserve balance */

    /* VLC provides the software volume so convert directly to PulseAudio
     * software volume, pa_volume_t. This is not a linear amplification factor
     * so do not use PulseAudio linear amplification! */
    vol *= PA_VOLUME_NORM;
    if (unlikely(vol >= PA_VOLUME_MAX))
        vol = PA_VOLUME_MAX;
    volume = pa_sw_volume_multiply(volume, lround(vol));
    pa_sw_cvolume_multiply_scalar(&cvolume, &cvolume, volume);

    assert(pa_cvolume_valid(&cvolume));

    vlc_pa_lock();
    op = pa_context_set_sink_input_volume(sys->context, idx, &cvolume, NULL, NULL);
    if (likely(op != NULL))
        pa_operation_unref(op);
    op = pa_context_set_sink_input_mute(sys->context, idx, mute, NULL, NULL);
    if (likely(op != NULL))
        pa_operation_unref(op);
    vlc_pa_unlock();

    return 0;
}

static int StreamMove(vlc_object_t *obj, const char *varname, vlc_value_t old,
                      vlc_value_t val, void *userdata)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = aout->sys;
    pa_stream *s = userdata;
    pa_operation *op;
    uint32_t idx = pa_stream_get_index(s);
    uint32_t sink_idx = val.i_int;

    (void) varname; (void) old;

    vlc_pa_lock();
    op = pa_context_move_sink_input_by_index(sys->context, idx, sink_idx,
                                             NULL, NULL);
    if (likely(op != NULL)) {
        pa_operation_unref(op);
        msg_Dbg(aout, "moving to sink %"PRIu32, sink_idx);
    } else
        vlc_pa_error(obj, "cannot move sink", sys->context);
    vlc_pa_unlock();

    return (op != NULL) ? VLC_SUCCESS : VLC_EGENERIC;
}


/**
 * Create a PulseAudio playback stream, a.k.a. a sink input.
 */
static int Open(vlc_object_t *obj)
{
#if !PA_CHECK_VERSION(0,9,22)
    if (!vlc_xlib_init(obj))
        return VLC_EGENERIC;
#endif

    audio_output_t *aout = (audio_output_t *)obj;
    pa_operation *op;

    /* Sample format specification */
    struct pa_sample_spec ss;
    vlc_fourcc_t format = aout->format.i_format;

    switch(format)
    {
        case VLC_CODEC_F64B:
            format = VLC_CODEC_F32B;
        case VLC_CODEC_F32B:
            ss.format = PA_SAMPLE_FLOAT32BE;
            break;
        case VLC_CODEC_F64L:
            format = VLC_CODEC_F32L;
        case VLC_CODEC_F32L:
            ss.format = PA_SAMPLE_FLOAT32LE;
            break;
        case VLC_CODEC_FI32:
            format = VLC_CODEC_FL32;
            ss.format = PA_SAMPLE_FLOAT32NE;
            break;
        case VLC_CODEC_S32B:
            ss.format = PA_SAMPLE_S32BE;
            break;
        case VLC_CODEC_S32L:
            ss.format = PA_SAMPLE_S32LE;
            break;
        case VLC_CODEC_S24B:
            ss.format = PA_SAMPLE_S24BE;
            break;
        case VLC_CODEC_S24L:
            ss.format = PA_SAMPLE_S24LE;
            break;
        case VLC_CODEC_S16B:
            ss.format = PA_SAMPLE_S16BE;
            break;
        case VLC_CODEC_S16L:
            ss.format = PA_SAMPLE_S16LE;
            break;
        case VLC_CODEC_S8:
            format = VLC_CODEC_U8;
        case VLC_CODEC_U8:
            ss.format = PA_SAMPLE_U8;
            break;
        default:
            if (HAVE_FPU)
            {
                format = VLC_CODEC_FL32;
                ss.format = PA_SAMPLE_FLOAT32NE;
            }
            else
            {
                format = VLC_CODEC_S16N;
                ss.format = PA_SAMPLE_S16NE;
            }
            break;
    }

    ss.rate = aout->format.i_rate;
    ss.channels = aout_FormatNbChannels(&aout->format);
    if (!pa_sample_spec_valid(&ss)) {
        msg_Err(aout, "unsupported sample specification");
        return VLC_EGENERIC;
    }

    /* Channel mapping (order defined in vlc_aout.h) */
    struct pa_channel_map map;
    map.channels = 0;

    if (aout->format.i_physical_channels & AOUT_CHAN_LEFT)
        map.map[map.channels++] = PA_CHANNEL_POSITION_FRONT_LEFT;
    if (aout->format.i_physical_channels & AOUT_CHAN_RIGHT)
        map.map[map.channels++] = PA_CHANNEL_POSITION_FRONT_RIGHT;
    if (aout->format.i_physical_channels & AOUT_CHAN_MIDDLELEFT)
        map.map[map.channels++] = PA_CHANNEL_POSITION_SIDE_LEFT;
    if (aout->format.i_physical_channels & AOUT_CHAN_MIDDLERIGHT)
        map.map[map.channels++] = PA_CHANNEL_POSITION_SIDE_RIGHT;
    if (aout->format.i_physical_channels & AOUT_CHAN_REARLEFT)
        map.map[map.channels++] = PA_CHANNEL_POSITION_REAR_LEFT;
    if (aout->format.i_physical_channels & AOUT_CHAN_REARRIGHT)
        map.map[map.channels++] = PA_CHANNEL_POSITION_REAR_RIGHT;
    if (aout->format.i_physical_channels & AOUT_CHAN_REARCENTER)
        map.map[map.channels++] = PA_CHANNEL_POSITION_REAR_CENTER;
    if (aout->format.i_physical_channels & AOUT_CHAN_CENTER)
    {
        if (ss.channels == 1)
            map.map[map.channels++] = PA_CHANNEL_POSITION_MONO;
        else
            map.map[map.channels++] = PA_CHANNEL_POSITION_FRONT_CENTER;
    }
    if (aout->format.i_physical_channels & AOUT_CHAN_LFE)
        map.map[map.channels++] = PA_CHANNEL_POSITION_LFE;

    for (unsigned i = 0; map.channels < ss.channels; i++) {
        map.map[map.channels++] = PA_CHANNEL_POSITION_AUX0 + i;
        msg_Warn(aout, "mapping channel %"PRIu8" to AUX%u", map.channels, i);
    }

    if (!pa_channel_map_valid(&map)) {
        msg_Err(aout, "unsupported channel map");
        return VLC_EGENERIC;
    } else {
        const char *name = pa_channel_map_to_name(&map);
        msg_Dbg(aout, "using %s channel map", (name != NULL) ? name : "?");
    }

    /* Stream parameters */
    const pa_stream_flags_t flags = PA_STREAM_START_CORKED
                                  //| PA_STREAM_INTERPOLATE_TIMING
                                  | PA_STREAM_AUTO_TIMING_UPDATE
                                  | PA_STREAM_VARIABLE_RATE;

    struct pa_buffer_attr attr;
    attr.maxlength = -1;
    /* PulseAudio assumes that tlength bytes are available in the buffer. Thus
     * we need to be conservative and set the minimum value that the VLC
     * audio decoder thread warrants. Otherwise, PulseAudio buffers will
     * underrun on hardware with large buffers. VLC keeps at least
     * AOUT_MIN_PREPARE and at most AOUT_MAX_PREPARE worth of audio buffers.
     * TODO? tlength could be adaptively increased to reduce wakeups. */
    attr.tlength = pa_usec_to_bytes(AOUT_MIN_PREPARE_TIME, &ss);
    attr.prebuf = 0; /* trigger manually */
    attr.minreq = -1;
    attr.fragsize = 0; /* not used for output */

    /* Allocate structures */
    aout_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    pa_context *ctx = vlc_pa_connect (obj);
    if (ctx == NULL)
    {
        free (sys);
        return VLC_EGENERIC;
    }

    aout->sys = sys;
    sys->stream = NULL;
    sys->context = ctx;
    sys->paused = VLC_TS_INVALID;
    sys->pts = VLC_TS_INVALID;
    sys->desync = 0;
    sys->rate = ss.rate;

    /* Context events */
    const pa_subscription_mask_t mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;

    pa_context_set_subscribe_callback(ctx, context_cb, aout);
    op = pa_context_subscribe(ctx, mask, NULL, NULL);
    if (likely(op != NULL))
       pa_operation_unref(op);

    /* Channel volume */
    sys->base_volume = PA_VOLUME_NORM;
    pa_cvolume_set(&sys->cvolume, ss.channels, PA_VOLUME_NORM);

    vlc_pa_lock();
    /* Create a playback stream */
    pa_stream *s = pa_stream_new(ctx, "audio stream", &ss, &map);
    if (s == NULL) {
        vlc_pa_error(obj, "stream creation failure", ctx);
        goto fail;
    }
    sys->stream = s;
    pa_stream_set_state_callback(s, stream_state_cb, NULL);
    pa_stream_set_latency_update_callback(s, stream_latency_cb, aout);
    pa_stream_set_moved_callback(s, stream_moved_cb, aout);
    pa_stream_set_overflow_callback(s, stream_overflow_cb, aout);
    pa_stream_set_started_callback(s, stream_started_cb, aout);
    pa_stream_set_suspended_callback(s, stream_suspended_cb, aout);
    pa_stream_set_underflow_callback(s, stream_underflow_cb, aout);

    if (pa_stream_connect_playback(s, NULL, &attr, flags, NULL, NULL) < 0
     || stream_wait(s)) {
        vlc_pa_error(obj, "stream connection failure", ctx);
        goto fail;
    }

    const struct pa_buffer_attr *pba = pa_stream_get_buffer_attr(s);
    msg_Dbg(aout, "using buffer metrics: maxlength=%u, tlength=%u, "
            "prebuf=%u, minreq=%u",
            pba->maxlength, pba->tlength, pba->prebuf, pba->minreq);

    var_Create(aout, "audio-device", VLC_VAR_INTEGER|VLC_VAR_HASCHOICE);
    var_Change(aout, "audio-device", VLC_VAR_SETTEXT,
               &(vlc_value_t){ .psz_string = (char *)_("Audio device") },
               NULL);
    var_AddCallback (aout, "audio-device", StreamMove, s);
    op = pa_context_get_sink_info_list(ctx, sink_list_cb, aout);
    /* We may need to wait for completion... once LibVLC supports this */
    if (op != NULL)
        pa_operation_unref(op);
    stream_moved_cb(s, aout);
    vlc_pa_unlock();

    aout->format.i_format = format;
    aout->pf_play = Play;
    aout->pf_pause = Pause;
    aout->pf_flush = Flush;
    aout_VolumeHardInit (aout, VolumeSet);
    return VLC_SUCCESS;

fail:
    vlc_pa_unlock();
    Close(obj);
    return VLC_EGENERIC;
}

/**
 * Removes a PulseAudio playback stream
 */
static void Close (vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = aout->sys;
    pa_context *ctx = sys->context;
    pa_stream *s = sys->stream;

    if (s != NULL) {
        /* The callback takes mainloop lock, so it CANNOT be held here! */
        var_DelCallback (aout, "audio-device", StreamMove, s);
        var_Destroy (aout, "audio-device");

        vlc_pa_lock ();
        pa_stream_disconnect(s);

        /* Clear all callbacks */
        pa_stream_set_state_callback(s, NULL, NULL);
        pa_stream_set_latency_update_callback(s, NULL, NULL);
        pa_stream_set_moved_callback(s, NULL, NULL);
        pa_stream_set_overflow_callback(s, NULL, NULL);
        pa_stream_set_started_callback(s, NULL, NULL);
        pa_stream_set_suspended_callback(s, NULL, NULL);
        pa_stream_set_underflow_callback(s, NULL, NULL);

        pa_stream_unref(s);
        vlc_pa_unlock ();
    }

    vlc_pa_disconnect(obj, ctx);
    free(sys);
}