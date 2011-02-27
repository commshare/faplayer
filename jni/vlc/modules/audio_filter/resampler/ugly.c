/*****************************************************************************
 * ugly.c : ugly resampler (changes pitch)
 *****************************************************************************
 * Copyright (C) 2002, 2006 the VideoLAN team
 * $Id$
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_filter.h>

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Create    ( vlc_object_t * );

static block_t *DoWork( filter_t *, block_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin ()
    set_description( N_("Audio filter for ugly resampling") )
    set_capability( "audio filter", 2 )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_MISC )
    set_callbacks( Create, NULL )
vlc_module_end ()

/*****************************************************************************
 * Create: allocate ugly resampler
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{
    filter_t * p_filter = (filter_t *)p_this;

    if ( p_filter->fmt_in.audio.i_rate == p_filter->fmt_out.audio.i_rate
          || p_filter->fmt_in.audio.i_format != p_filter->fmt_out.audio.i_format
          || p_filter->fmt_in.audio.i_physical_channels
              != p_filter->fmt_out.audio.i_physical_channels
          || p_filter->fmt_in.audio.i_original_channels
              != p_filter->fmt_out.audio.i_original_channels
          || (p_filter->fmt_in.audio.i_format != VLC_CODEC_FL32
               && p_filter->fmt_in.audio.i_format != VLC_CODEC_FI32) )
    {
        return VLC_EGENERIC;
    }

    p_filter->pf_audio_filter = DoWork;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * DoWork: convert a buffer
 *****************************************************************************/
static block_t *DoWork( filter_t * p_filter, block_t * p_in_buf )
{
    /* Check if we really need to run the resampler */
    if( p_filter->fmt_out.audio.i_rate == p_filter->fmt_in.audio.i_rate )
        return p_in_buf;

    block_t *p_out_buf = p_in_buf;
    unsigned int i_out_nb = p_in_buf->i_nb_samples
        * p_filter->fmt_out.audio.i_rate / p_filter->fmt_in.audio.i_rate;
    const unsigned int i_sample_bytes =
        aout_FormatNbChannels( &p_filter->fmt_in.audio ) * sizeof(int32_t);

    if( p_filter->fmt_out.audio.i_rate > p_filter->fmt_in.audio.i_rate )
    {
        p_out_buf = block_Alloc( i_out_nb * i_sample_bytes );
        if( !p_out_buf )
            goto out;
    }

    unsigned char *p_out = p_out_buf->p_buffer;
    unsigned char *p_in = p_in_buf->p_buffer;
    unsigned int i_remainder = 0;

    p_out_buf->i_nb_samples = i_out_nb;
    p_out_buf->i_buffer = i_out_nb * i_sample_bytes;
    p_out_buf->i_pts = p_in_buf->i_pts;
    p_out_buf->i_length = p_out_buf->i_nb_samples *
        1000000 / p_filter->fmt_out.audio.i_rate;

    while( i_out_nb )
    {
        if( p_out != p_in )
            memcpy( p_out, p_in, i_sample_bytes );
        p_out += i_sample_bytes;
        i_out_nb--;

        i_remainder += p_filter->fmt_in.audio.i_rate;
        while( i_remainder >= p_filter->fmt_out.audio.i_rate )
        {
            p_in += i_sample_bytes;
            i_remainder -= p_filter->fmt_out.audio.i_rate;
        }
    }

    if( p_in_buf != p_out_buf )
out:
        block_Release( p_in_buf );
    return p_out_buf;
}
