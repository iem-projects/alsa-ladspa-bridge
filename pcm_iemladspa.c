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

static void print_pcm_extplug(snd_pcm_extplug_t*ext) {

  printf("EXTPLUG: %p\n", ext);
  printf("EXTPLUG: name=%s\n", ext->name);
  printf("EXTPLUG: version=%d\n", ext->version);
  printf("EXTPLUG: callback=%p\n", ext->callback);
  printf("EXTPLUG: private_data=%p\n", ext->private_data);
  printf("EXTPLUG: pcm=%p\n", ext->pcm);
  printf("EXTPLUG: stream=%d\n", ext->stream);
  printf("EXTPLUG: format=%d\n", ext->format);
  printf("EXTPLUG: subformat=%d\n", ext->subformat);
  printf("EXTPLUG: channels=%d\n", ext->channels);
  printf("EXTPLUG: rate=%d\n", ext->rate);
  printf("EXTPLUG: slave_format=%d\n", ext->slave_format);
  printf("EXTPLUG: slave_subformat=%d\n", ext->slave_subformat);
  printf("EXTPLUG: slave_channels=%d\n", ext->slave_channels);
}

typedef struct snd_pcm_iemladspa {
	snd_pcm_extplug_t ext;
	void *library;
	const LADSPA_Descriptor *klass;
	LADSPA_Control *control_data;
	LADSPA_Handle *channelinstance;

} snd_pcm_iemladspa_t;

static inline void interleave(float *src, float *dst, int frames, int channels)
{
	int i, j;
	for(i = 0; i < frames; i++){
		for(j = 0; j < channels; j++){
			dst[i*channels + j] = src[i + frames*j];
		}
	}
}

static inline void deinterleave(float *src, float *dst, int frames, int channels)
{
	int i, j;
	for(i = 0; i < frames; i++){
		for(j = 0; j < channels; j++){
			dst[i + frames*j] = src[i*channels + j];
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
  const unsigned long offset_in = iemladspa->control_data->num_controls;
  const unsigned long offset_out = offset_in + iemladspa->control_data->num_inchannels;
  const int channels = iemladspa->control_data->num_inchannels + iemladspa->control_data->num_outchannels;

	/* Calculate buffer locations */
	src = (float*)(src_areas->addr +
			(src_areas->first + src_areas->step * src_offset)/8);
	dst = (float*)(dst_areas->addr +
			(dst_areas->first + dst_areas->step * dst_offset)/8);	

#if 0
  printf("transfer: %p from %p to %p\n", ext, src_areas, dst_areas);
  printf("transferring from %p to %p\n", src, dst);
#endif

	/* NOTE: swap source and destination memory space when deinterleaved.
		then swap it back during the interleave call below */
  deinterleave(src, dst, size, channels);


	for(j = 0; j < iemladspa->control_data->num_inchannels; j++) {
    printf("connect  inport %d to %p\n", iemladspa->control_data->data[offset_in + j].index,  dst + j*size);
		iemladspa->klass->connect_port(iemladspa->channelinstance,
                                 iemladspa->control_data->data[offset_in + j].index,
                                 dst + j*size);
  }
	for(j = 0; j < iemladspa->control_data->num_outchannels; j++) {
    printf("connect outport %d to %p\n", iemladspa->control_data->data[offset_out+ j].index, src + j*size);
		iemladspa->klass->connect_port(iemladspa->channelinstance,
                                 iemladspa->control_data->data[offset_out+ j].index,
                                 src + j*size);
  }

  iemladspa->klass->run(iemladspa->channelinstance, size);
	
	interleave(src, dst, size, channels);

	return size;
}

static int iemladspa_close(snd_pcm_extplug_t *ext) {
	snd_pcm_iemladspa_t *iemladspa = ext->private_data;
  printf("closing: %p", ext);
  print_pcm_extplug(ext);

  if(iemladspa->klass->deactivate) {
    iemladspa->klass->deactivate(iemladspa->channelinstance);
  }


  /* TODO: Figure out why this segfaults */
#if 1
  if(iemladspa->klass->cleanup) {
    iemladspa->klass->cleanup(iemladspa->channelinstance);
  }
#endif

	LADSPAcontrolUnMMAP(iemladspa->control_data);
	LADSPAunload(iemladspa->library);
	free(iemladspa);
	return 0;
}

static int iemladspa_init(snd_pcm_extplug_t *ext)
{
	snd_pcm_iemladspa_t *iemladspa = (snd_pcm_iemladspa_t *)ext;
	int i;

  printf("init %p playback=%d\n", ext, ext->stream==SND_PCM_STREAM_PLAYBACK);
  print_pcm_extplug(ext);

	/* Instantiate a LADSPA Plugin */

  iemladspa->channelinstance = iemladspa->klass->instantiate(iemladspa->klass, ext->rate);
  if(iemladspa->channelinstance == NULL) {
    return -1;
  }
  if(iemladspa->klass->activate) {
    iemladspa->klass->activate(iemladspa->channelinstance);
  }

	/* Connect controls to the LADSPA Plugin */
  for(i = 0; i < iemladspa->control_data->num_controls; i++) {
    iemladspa->klass->connect_port(iemladspa->channelinstance,
                                 iemladspa->control_data->data[i].index,
                                 &iemladspa->control_data->data[i].data);
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
	const char *library = "/usr/lib/ladspa/iemladspa.so";
	const char *module = "iemladspa";
	int err;
  long  inchannels = 2;
  long outchannels = 2;


  printf("stream=%d\n", stream);
	
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
		if (strcmp(id, "inchannels") == 0) {
      snd_config_get_integer(n, &inchannels);
      if(inchannels < 1) {
        SNDERR("inchannels < 1");
        return -EINVAL;
      }
      continue;
		}
		if (strcmp(id, "outchannels") == 0) {
      snd_config_get_integer(n, &outchannels);
      if(outchannels < 1) {
        SNDERR("outchannels < 1");
        return -EINVAL;
      }
      continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

  printf("channels= %d = %d+%d\n", (int)(inchannels+outchannels), (int)inchannels, (int)outchannels);

  if(1) {
    const void*id=NULL;
    snd_config_get_pointer(sconf, &id);
    printf("SLAVE: %p = %d\n", id, snd_config_get_type(sconf));
  }
	/* Make sure we have a slave and control devices defined */
	if (! sconf) {
		SNDERR("No slave configuration for iemladspa pcm");
		return -EINVAL;
	}

	/* Intialize the local object data */
	iemladspa = calloc(1, sizeof(*iemladspa) + (inchannels+outchannels)*sizeof(LADSPA_Handle *));
	if (iemladspa == NULL)
		return -ENOMEM;

	iemladspa->ext.version = SND_PCM_EXTPLUG_VERSION;
	iemladspa->ext.name = "alsaiemladspa";
	iemladspa->ext.callback = &iemladspa_callback;
	iemladspa->ext.private_data = iemladspa;

	/* Open the LADSPA Plugin */
	iemladspa->library = LADSPAload(library);
  printf("LADSPAlib: %p\n", iemladspa->library);
	if(iemladspa->library == NULL) {
		return -1;
	}

	iemladspa->klass = LADSPAfind(iemladspa->library, library, module);
  printf("LADSPAklass: %p\n", iemladspa->klass);

	if(iemladspa->klass == NULL) {
		return -1;
	}

	/* Create the ALSA External Plugin */
  printf("creating external plugin\n");
	err = snd_pcm_extplug_create(&iemladspa->ext, name, root, sconf, stream, mode);
	if (err < 0) {
    printf("extplug failed\n");
		return err;
	}
  printf("plugin: %p @ %d channels\n", iemladspa, (int)(inchannels+outchannels));

  printf("ROOT:\n");
  print_pcm_extplug(&iemladspa->ext);
  printf(":ROOT \n");


	/* MMAP to the controls file */
  iemladspa->control_data = LADSPAcontrolMMAP(iemladspa->klass, controls, inchannels, outchannels);
	if(iemladspa->control_data == NULL) {
		return -1;
	}

	/* Make sure that the control file makes sense */
  unsigned int j;

  const unsigned long offset_in = iemladspa->control_data->num_controls;
  const unsigned long offset_out = offset_in + iemladspa->control_data->num_inchannels;

  for(j=0; j<iemladspa->control_data->num_inchannels; j++) {
    unsigned int index=iemladspa->control_data->data[offset_in + j].index;
    if(index>=iemladspa->klass->PortCount || iemladspa->klass->PortDescriptors[index] !=
       (LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO)) {
      SNDERR("Problem with control file %s.", controls);
      return -1;
    }
  }
  for(j=0; j<iemladspa->control_data->num_outchannels; j++) {
    unsigned int index=iemladspa->control_data->data[offset_out+ j].index;

    if(index>=iemladspa->klass->PortCount || iemladspa->klass->PortDescriptors[index] !=
       (LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO)) {
      SNDERR("Problem with control file %s.", controls);
      return -1;
    }
  }

	/* Set PCM Contraints */
#if 0
	snd_pcm_extplug_set_param_minmax(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_CHANNELS,
			iemladspa->control_data->channels,
			iemladspa->control_data->channels);

	snd_pcm_extplug_set_slave_param(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_CHANNELS,
			iemladspa->control_data->channels);
#endif

	snd_pcm_extplug_set_param(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT);

	snd_pcm_extplug_set_slave_param(&iemladspa->ext,
			SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT);

	*pcmp = iemladspa->ext.pcm;
	
	return 0;

}

SND_PCM_PLUGIN_SYMBOL(iemladspa);

