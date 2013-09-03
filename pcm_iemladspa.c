/*
 * Copyright (c) 2008 Cooper Street Innovations
 * 		<charles@cooper-street.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <stdio.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm.h>
#include <alsa/pcm_external.h>
#include <alsa/control.h>
#include <linux/soundcard.h>

#include <ladspa.h>
#include "ladspa_utils.h"

typedef struct snd_pcm_iemladspa {
	snd_pcm_extplug_t ext;
	void *library;
	const LADSPA_Descriptor *klass;
	LADSPA_Control *control_data;
	LADSPA_Handle *channel[];
} snd_pcm_iemladspa_t;

static inline void interleave(float *src, float *dst, int n, int m)
{
	int i, j;
	for(i = 0; i < n; i++){
		for(j = 0; j < m; j++){
			dst[i*m + j] = src[i + n*j];
		}
	}
}

static inline void deinterleave(float *src, float *dst, int n, int m)
{
	int i, j;
	for(i = 0; i < n; i++){
		for(j = 0; j < m; j++){
			dst[i + n*j] = src[i*m + j];
		}
	}
}

static snd_pcm_sframes_t iemladspa_transfer(snd_pcm_extplug_t *ext,
		  const snd_pcm_channel_area_t *dst_areas,
		  snd_pcm_uframes_t dst_offset,
		  const snd_pcm_channel_area_t *src_areas,
		  snd_pcm_uframes_t src_offset,
		  snd_pcm_uframes_t size)
{
	snd_pcm_iemladspa_t *iemladspa = (snd_pcm_iemladspa_t *)ext;
	float *src, *dst;
	int j;
	
	/* Calculate buffer locations */
	src = (float*)(src_areas->addr +
			(src_areas->first + src_areas->step * src_offset)/8);
	dst = (float*)(dst_areas->addr +
			(dst_areas->first + dst_areas->step * dst_offset)/8);	
	
	/* NOTE: swap source and destination memory space when deinterleaved.
		then swap it back during the interleave call below */
	deinterleave(src, dst, size, iemladspa->control_data->channels);
	
	for(j = 0; j < iemladspa->control_data->channels; j++) {
		iemladspa->klass->connect_port(iemladspa->channel[j],
			iemladspa->control_data->input_index,
			dst + j*size);
		iemladspa->klass->connect_port(iemladspa->channel[j],
			iemladspa->control_data->output_index,
			src + j*size);
		iemladspa->klass->run(iemladspa->channel[j], size);
	}
	
	interleave(src, dst, size, iemladspa->control_data->channels);

	return size;
}

static int iemladspa_close(snd_pcm_extplug_t *ext) {
	snd_pcm_iemladspa_t *iemladspa = ext->private_data;
	int i;
	for (i = 0; i < iemladspa->control_data->channels; i++) {
		if(iemladspa->klass->deactivate) {
			iemladspa->klass->deactivate(iemladspa->channel[i]);
		}
		/* TODO: Figure out why this segfaults */
		/* if(iemladspa->klass->cleanup) {
			iemladspa->klass->cleanup(iemladspa->channel[i]);
		} */
	}
	LADSPAcontrolUnMMAP(iemladspa->control_data);
	LADSPAunload(iemladspa->library);
	free(iemladspa);
	return 0;
}

static int iemladspa_init(snd_pcm_extplug_t *ext)
{
	snd_pcm_iemladspa_t *iemladspa = (snd_pcm_iemladspa_t *)ext;
	int i, j;

	/* Instantiate a LADSPA Plugin for each channel */
	for(i = 0; i < iemladspa->control_data->channels; i++) {
		iemladspa->channel[i] = iemladspa->klass->instantiate(
				iemladspa->klass, ext->rate);
		if(iemladspa->channel[i] == NULL) {
			return -1;
		}
		if(iemladspa->klass->activate) {
			iemladspa->klass->activate(iemladspa->channel[i]);
		}
	}

	/* Connect controls to the LADSPA Plugin */
	for(j = 0; j < iemladspa->control_data->channels; j++) {
		for(i = 0; i < iemladspa->control_data->num_controls; i++) {
			iemladspa->klass->connect_port(iemladspa->channel[j], 
					iemladspa->control_data->control[i].index,
					&iemladspa->control_data->control[i].data[j]);
		}
	}

	return 0;
}

static snd_pcm_extplug_callback_t iemladspa_callback = {
	.transfer = iemladspa_transfer,
	.init = iemladspa_init,
	.close = iemladspa_close,
};

SND_PCM_PLUGIN_DEFINE_FUNC(iemladspa)
{
	snd_config_iterator_t i, next;
	snd_pcm_iemladspa_t *iemladspa;
	snd_config_t *sconf = NULL;
	const char *controls = ".alsaiemladspa.bin";
	const char *library = "/usr/lib/ladspa/caps.so";
	const char *module = "Eq";
	long channels = 2;
	int err;
	
	/* Parse configuration options from asoundrc */
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
			continue;
		if (strcmp(id, "slave") == 0) {
			sconf = n;
			continue;
		}
		if (strcmp(id, "controls") == 0) {
			snd_config_get_string(n, &controls);
			continue;
		}
		if (strcmp(id, "library") == 0) {
			snd_config_get_string(n, &library);
			continue;
		}
		if (strcmp(id, "module") == 0) {
			snd_config_get_string(n, &module);
			continue;
		}
		if (strcmp(id, "channels") == 0) {
			snd_config_get_integer(n, &channels);
			if(channels < 1) {
				SNDERR("channels < 1");
				return -EINVAL;
			}
			continue;
		}
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	/* Make sure we have a slave and control devices defined */
	if (! sconf) {
		SNDERR("No slave configuration for iemladspa pcm");
		return -EINVAL;
	}

	/* Intialize the local object data */
	iemladspa = calloc(1, sizeof(*iemladspa) + channels*sizeof(LADSPA_Handle *));
	if (iemladspa == NULL)
		return -ENOMEM;

	iemladspa->ext.version = SND_PCM_EXTPLUG_VERSION;
	iemladspa->ext.name = "alsaiemladspa";
	iemladspa->ext.callback = &iemladspa_callback;
	iemladspa->ext.private_data = iemladspa;

	/* Open the LADSPA Plugin */
	iemladspa->library = LADSPAload(library);
	if(iemladspa->library == NULL) {
		return -1;
	}

	iemladspa->klass = LADSPAfind(iemladspa->library, library, module);
	if(iemladspa->klass == NULL) {
		return -1;
	}

	/* Create the ALSA External Plugin */
	err = snd_pcm_extplug_create(&iemladspa->ext, name, root, sconf, stream, mode);
	if (err < 0) {
		return err;
	}

	/* MMAP to the controls file */
	iemladspa->control_data = LADSPAcontrolMMAP(iemladspa->klass, controls, channels);
	if(iemladspa->control_data == NULL) {
		return -1;
	}

	/* Make sure that the control file makes sense */
	if(iemladspa->klass->PortDescriptors[iemladspa->control_data->input_index] !=
			(LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO)) {
		SNDERR("Problem with control file %s.", controls);
		return -1;
	}
	if(iemladspa->klass->PortDescriptors[iemladspa->control_data->output_index] !=
			(LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO)) {
		SNDERR("Problem with control file %s.", controls);
		return -1;
	}

	/* Set PCM Contraints */
	snd_pcm_extplug_set_param_minmax(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_CHANNELS,
			iemladspa->control_data->channels,
			iemladspa->control_data->channels);
	snd_pcm_extplug_set_slave_param(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_CHANNELS,
			iemladspa->control_data->channels);
	snd_pcm_extplug_set_param(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT);
	snd_pcm_extplug_set_slave_param(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT);

	*pcmp = iemladspa->ext.pcm;
	
	return 0;

}

SND_PCM_PLUGIN_SYMBOL(iemladspa);

