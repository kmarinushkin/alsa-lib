/*
 *  Rate conversion Plug-In
 *  Copyright (c) 1999 by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
  
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <byteswap.h>
#include "../pcm_local.h"

#define SHIFT	11
#define BITS	(1<<SHIFT)
#define MASK	(BITS-1)
#define MAX_VOICES 6

/*
 *  Basic rate conversion plugin
 */
 
struct rate_private_data {
	int src_voices;
	int dst_voices;
	int src_rate;
	int dst_rate;
	unsigned int pitch;
	unsigned int pos;
	signed short last_S1[MAX_VOICES];
	signed short last_S2[MAX_VOICES];
	ssize_t old_src_size, old_dst_size;
};


static void mix(struct rate_private_data *data, int voices,
		signed short *src_ptr, int src_size,
		signed short *dst_ptr, int dst_size)
{
	unsigned int pos;
	signed int val;
	signed short *S1, *S2;
	int voice;
	
	pos = data->pos;
	S1 = data->last_S1;
	S2 = data->last_S2;
	if (pos >> SHIFT) {
		src_ptr += ((pos >> SHIFT) - 1) * voices; pos &= MASK;
		for (voice = 0; voice < voices; ++voice) {
			S1[voice] = S2[voice];
			S2[voice] = src_ptr[voice];
		}
	}
	while (dst_size-- > 0) {
		if (pos >> SHIFT) {
			src_ptr += (pos >> SHIFT) * voices; pos &= MASK;
			for (voice = 0; voice < voices; ++voice) {
				S1[voice] = S2[voice];
				S2[voice] = src_ptr[voice];
			}
		}
		
		for (voice = 0; voice < voices; ++voice) {
			val = S1[voice] + ((S2[voice] - S1[voice]) * (signed int)pos) / BITS;
			if (val < -32768)
				val = -32768;
			else if (val > 32767)
				val = 32767;
			*dst_ptr++ = val;
		}
		
		pos += data->pitch;
	}
	data->pos = pos;
}

static ssize_t rate_transfer(snd_pcm_plugin_t *plugin,
			     char *src_ptr, size_t src_size,
			     char *dst_ptr, size_t dst_size)
{
	struct rate_private_data *data;

	if (plugin == NULL || src_ptr == NULL || src_size < 0 ||
	                      dst_ptr == NULL || dst_size < 0)
		return -EINVAL;
	if (src_size == 0)
		return 0;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	if (data == NULL)
		return -EINVAL;
	mix(data, data->src_voices,
	    (signed short *)src_ptr, src_size / (data->src_voices * 2),
	    (signed short *)dst_ptr, dst_size / (data->dst_voices * 2));
	return dst_size / (2 * data->src_voices) * (2 * data->src_voices);
}

static int rate_action(snd_pcm_plugin_t *plugin, snd_pcm_plugin_action_t action)
{
	struct rate_private_data *data;
	int voice;

	if (plugin == NULL)
		return -EINVAL;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	switch (action) {
	case INIT:
	case PREPARE:
	case DRAIN:
	case FLUSH:
		data->pos = 0;
		for (voice = 0; voice < data->src_voices; ++voice) {
			data->last_S1[voice] = data->last_S2[voice] = 0;
		}
		break;
	}
	return 0;	/* silenty ignore other actions */
}

static ssize_t rate_src_size(snd_pcm_plugin_t *plugin, size_t size)
{
	struct rate_private_data *data;
	ssize_t res;

	if (!plugin || size <= 0)
		return -EINVAL;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	res = (((size * data->pitch) + (BITS/2)) >> SHIFT);
	res = res / (data->src_voices*2) * (data->src_voices*2);
	/* Why this? */
	if (size < 128*1024) {
		if (data->old_src_size == size)
			return data->old_dst_size;
		data->old_src_size = size;
		data->old_dst_size = res;
	}
	return res;
}

static ssize_t rate_dst_size(snd_pcm_plugin_t *plugin, size_t size)
{
	struct rate_private_data *data;
	ssize_t res;

	if (!plugin || size <= 0)
		return -EINVAL;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	res = (((size << SHIFT) + (data->pitch / 2)) / data->pitch);
	res = res / (data->dst_voices*2) * (data->dst_voices*2);
	/* Why this? */
	if (size < 128*1024) {
		if (data->old_dst_size == size)
			return data->old_src_size;
		data->old_dst_size = size;
		data->old_src_size = res;
	}
	return res;
}

int snd_pcm_plugin_build_rate(snd_pcm_format_t *src_format,
			      snd_pcm_format_t *dst_format,
			      snd_pcm_plugin_t **r_plugin)
{
	struct rate_private_data *data;
	snd_pcm_plugin_t *plugin;
	int voice;

	if (!r_plugin)
		return -EINVAL;
	*r_plugin = NULL;

	if (src_format->interleave != dst_format->interleave)
		return -EINVAL;
	if (src_format->format != dst_format->format)
		return -EINVAL;
	if (!dst_format->interleave)
		return -EINVAL;
	if (src_format->voices != dst_format->voices)
		return -EINVAL;
	if (dst_format->voices < 1 || dst_format->voices > MAX_VOICES)
		return -EINVAL;

	if (src_format->format != SND_PCM_SFMT_S16_LE ||
	    dst_format->format != SND_PCM_SFMT_S16_LE)
		return -EINVAL;
	if (src_format->rate == dst_format->rate)
		return -EINVAL;
	plugin = snd_pcm_plugin_build("rate format conversion",
				      sizeof(struct rate_private_data));
	if (plugin == NULL)
		return -ENOMEM;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	data->src_voices = src_format->voices;
	data->dst_voices = dst_format->voices;
	data->src_rate = src_format->rate;
	data->dst_rate = dst_format->rate;
	data->pitch = ((src_format->rate << SHIFT) + (dst_format->rate >> 1)) / dst_format->rate;
	data->pos = 0;
	for (voice = 0; voice < data->src_voices; ++voice) {
		data->last_S1[voice] = data->last_S2[voice] = 0;
	}
	data->old_src_size = data->old_dst_size = 0;
	plugin->transfer = rate_transfer;
	plugin->src_size = rate_src_size;
	plugin->dst_size = rate_dst_size;
	plugin->action = rate_action;
	*r_plugin = plugin;
	return 0;
}
