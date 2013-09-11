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

/* nomenclature of in/out,...
 *    inchannel    : read-only "input" data (as produced by a microphone or an application, before being passed to us)
 *    outchannel   : write-only "output" data (will be send to soundcard or an application, generated by us)
 *    sourcechannel: channel being passed from soundcard to application
 *    sinkchannel  : channel being passed from application to soundcard (has the same number of in- and outchannels)
 *
 * the LADSPA-plugin must have
 *    (source.inchannels +sink.inchannels ) inputs
 *    (source.outchannels+sink.outchannels) outputs
 *
 *    for simplicity reasons source.inchannels==source.outchannels (same for sink)
 */

#include <stdio.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm.h>
#include <alsa/pcm_external.h>
#include <alsa/control.h>
#include <linux/soundcard.h>

#include <ladspa.h>
#include "ladspa_utils.h"

#define DEBUG() printf("%s:%d\t%s\n", __FILE__, __LINE__, __FUNCTION__)

typedef struct _iemladspa_audiobuf {
  unsigned int frames;
  unsigned int channels;
  size_t size;
  float *data;
} iemladspa_audiobuf_t;

typedef struct snd_pcm_iemladspa {
  unsigned int usecount;
	snd_pcm_extplug_t extIN, extOUT;

  snd_config_t   *sndconfig;

	void *library;
	const LADSPA_Descriptor *klass;

  iemladspa_audiobuf_t inbuf ;
  iemladspa_audiobuf_t outbuf;
  int stream_direction;

	LADSPA_Control *control_data;
	LADSPA_Handle *plugininstance;
} snd_pcm_iemladspa_t;

typedef struct linked_list {
  void*key;
  void*data;
  struct linked_list *next;
} linked_list_t;

void*linked_list_find(linked_list_t*list, void*key) {
  while(list) {
    if(list->key == key)
      return list->data;
    list=list->next;
  }
  return NULL;
}
linked_list_t*linked_list_add(linked_list_t*list, void*key, void*data) {
  linked_list_t*entry=(linked_list_t*)calloc(1, sizeof(linked_list_t));
  entry->key=key;
  entry->data=data;
  entry->next=list;

  return entry;
}
linked_list_t*linked_list_delete(linked_list_t*inlist, void*key) {
  linked_list_t*list=inlist, *last=NULL;
  while(list) {
    if(list->key == key) {
      /* found element, now delete it, repair the list and return it */
      linked_list_t*next=list->next;
      if(last) {
        last->next=next;
      } else {
        inlist=list->next;
      }

      list->key=NULL;
      list->data=NULL;
      list->next=NULL;
      free(list);

      return inlist;
    }
    last=list;
    list=list->next;
  }
  return inlist;
}
static linked_list_t*s_mergeplugin_list = NULL;

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
  printf("\n");
}

static void print_pcm_config(snd_config_t*config, const char*name) {
  int err;
  const char*str;

  printf("SNDCONFIG[%p]: '%s'=[%d]\n", config, name, snd_config_get_type(config));
  err=snd_config_get_id(config, &str);
  printf("SNDCONFIG[%p]: id[%d]=%s\n", config, err, str);
  err=snd_config_get_string(config, &str);
  printf("SNDCONFIG[%p]: string[%d]=%s\n", config, err, str);
  printf("\n");
}

static void audiobuffer_free(iemladspa_audiobuf_t *iemladspa) {
  if(iemladspa->data)free(iemladspa->data);
  iemladspa->data=NULL;
  iemladspa->frames=0;
  iemladspa->channels=0;
  iemladspa->size=0;
}

static int audiobuffer_resize(iemladspa_audiobuf_t *buf, unsigned int frames, unsigned int channels) {
  size_t size=frames*channels;
  if(size > buf->size) {
    float*data=NULL;
    size*=2; /* over-allocation */
    data=realloc(buf->data, size*sizeof(float));
    if(!data) {
      audiobuffer_free(buf);
      return 0;
    }
    buf->data=data;
    buf->size = size;
  }
  if(frames != buf->frames || channels != buf->channels) {
    /* frames/channels have changed, clear the buffer */
    memset(buf->data, 0, size*sizeof(float));
  }
  buf->frames=frames;
  buf->channels=channels;
  return 1;
}
static inline void reinterleaveFLOAT(float *src, void *dst_, int frames, int channels)
{
  float*dst=(float*)dst_;
	int i, j;
	for(i = 0; i < frames; i++){
		for(j = 0; j < channels; j++){
			dst[i*channels + j] = src[i + frames*j];
		}
	}
}
static inline void deinterleaveFLOAT(void *src_, float *dst, int frames, int channels)
{
  float*src=(float*)src_;
	int i, j;
	for(i = 0; i < frames; i++){
		for(j = 0; j < channels; j++){
      dst[i + frames*j] = src[i*channels + j];
		}
	}
}
static inline void reinterleaveS16(float *src, void *dst_, int frames, int channels)
{
  signed short*dst=(signed short*)dst_;
	int i, j;
	for(i = 0; i < frames; i++){
		for(j = 0; j < channels; j++){
      int v = src[i + frames*j] * 32767.;
      if(v > 32767)
        v=32767;
      else if (v < -32767)
        v=-32767;
			dst[i*channels + j] = v;
		}
	}
}

static inline void deinterleaveS16(void *src_, float *dst, int frames, int channels)
{
  signed short*src=(signed short*)src_;
  const float scale = 1./32767.;
	int i, j;
	for(i = 0; i < frames; i++){
		for(j = 0; j < channels; j++){
      dst[i + frames*j] = src[i*channels + j] * scale;
		}
	}
}


typedef void reinterleave_fun_t(float *src, void *dst_, int frames, int channels);
typedef void deinterleave_fun_t(void *src_, float *dst, int frames, int channels);

static inline void connect_port(snd_pcm_iemladspa_t *iemladspa,
                         unsigned long Port,
                         LADSPA_Data * DataLocation,
                         const char*name) {
  //printf("connect %s\t %lu to %p\n", name, Port, DataLocation);
  iemladspa->klass->connect_port(iemladspa->plugininstance, Port, DataLocation);
}

static snd_pcm_sframes_t iemladspa_transfer(snd_pcm_extplug_t *ext,
		  const snd_pcm_channel_area_t *dst_areas,
		  snd_pcm_uframes_t dst_offset,
		  const snd_pcm_channel_area_t *src_areas,
		  snd_pcm_uframes_t src_offset,
		  snd_pcm_uframes_t size)
{
	snd_pcm_iemladspa_t *iemladspa = (snd_pcm_iemladspa_t *)(ext->private_data);
  const int playback = (SND_PCM_STREAM_PLAYBACK == ext->stream);

	int j;
  const unsigned long dataoffset_in  = iemladspa->control_data->num_controls;
  const unsigned long dataoffset_out = dataoffset_in + iemladspa->control_data->num_inchannels;

  const unsigned int inchannels  = (playback)?(iemladspa->control_data->sourcechannels.in ):(iemladspa->control_data->sinkchannels.in);
  const unsigned int outchannels = (playback)?(iemladspa->control_data->sourcechannels.out):(iemladspa->control_data->sinkchannels.out);

  const unsigned long bufoffset_in  = (playback)?(iemladspa->control_data->sourcechannels.in  * size):0;
  const unsigned long bufoffset_out = (playback)?(iemladspa->control_data->sourcechannels.out * size):0;

	/* Calculate buffer locations */
  /* first&step are given in bits, hence we device by 8
   */
	void *src = (src_areas->addr +
			(src_areas->first + src_areas->step * src_offset)/8);
	void *dst = (dst_areas->addr +
			(dst_areas->first + dst_areas->step * dst_offset)/8);	

  deinterleave_fun_t*deinterleave = NULL;
  reinterleave_fun_t*reinterleave = NULL;
  switch(ext->format) {
  case SND_PCM_FORMAT_FLOAT:
    deinterleave=deinterleaveFLOAT;
    reinterleave=reinterleaveFLOAT;
    break;
  case SND_PCM_FORMAT_S16:
    deinterleave=deinterleaveS16;
    reinterleave=reinterleaveS16;
    break;
  default:
    break;
  }

  if(!deinterleave || !reinterleave)return size;

  /* make sure out deinterleaving buffers are large enough */
  audiobuffer_resize(&iemladspa->inbuf , size,
                     iemladspa->control_data->sourcechannels.in +iemladspa->control_data->sinkchannels.in);
  audiobuffer_resize(&iemladspa->outbuf, size,
                     iemladspa->control_data->sourcechannels.out+iemladspa->control_data->sinkchannels.out);

	/* NOTE: swap source and destination memory space when deinterleaved.
     then swap it back during the interleave call below */
  deinterleave(src,
               iemladspa->inbuf.data + bufoffset_in,
               size, inchannels);


  /* only run when stream is in playback mode */
  if(playback) {
    for(j = 0; j < iemladspa->control_data->num_inchannels; j++) {
      connect_port(iemladspa,
                   iemladspa->control_data->data[dataoffset_in + j].index,
                   iemladspa->inbuf.data + j*size,
                   "inport "
                   );
    }
    for(j = 0; j < iemladspa->control_data->num_outchannels; j++) {
      connect_port(iemladspa,
                   iemladspa->control_data->data[dataoffset_out+ j].index,
                   iemladspa->outbuf.data + j*size,
                   "outport");
    }

    iemladspa->klass->run(iemladspa->plugininstance, size);
  }

	reinterleave(iemladspa->outbuf.data + bufoffset_out,
               dst,
               size, outchannels);

  iemladspa->stream_direction = ext->stream;
	return size;
}

static int iemladspa_close(snd_pcm_extplug_t *ext) {
	snd_pcm_iemladspa_t *iemladspa = (snd_pcm_iemladspa_t*)ext->private_data;

  /* check whether we are the last user of iemladspa */
  if((--(iemladspa->usecount))>0)
    return 0;

  if(iemladspa->plugininstance) {
    if(iemladspa->klass->deactivate) {
      iemladspa->klass->deactivate(iemladspa->plugininstance);
    }

    /* TODO: Figure out why this segfaults */
    if(iemladspa->klass->cleanup) {
      iemladspa->klass->cleanup(iemladspa->plugininstance);
    } else {
      free(iemladspa->plugininstance);
    }
  }
  iemladspa->plugininstance = NULL;


	if(iemladspa->control_data)
    LADSPAcontrolUnMMAP(iemladspa->control_data);
  iemladspa->control_data=NULL;
  if(iemladspa->library)
    LADSPAunload(iemladspa->library);
  iemladspa->library=NULL;

  s_mergeplugin_list = linked_list_delete(s_mergeplugin_list, iemladspa->sndconfig);
	free(iemladspa);
	return 0;
}

static int iemladspa_init(snd_pcm_extplug_t *ext)
{
	snd_pcm_iemladspa_t *iemladspa = (snd_pcm_iemladspa_t *)ext->private_data;
	int i;

  if(!iemladspa->plugininstance) {
    /* Instantiate a LADSPA Plugin */
    iemladspa->plugininstance=iemladspa->klass->instantiate(iemladspa->klass, ext->rate);

    if(iemladspa->plugininstance == NULL) {
      return -1;
    }
    if(iemladspa->klass->activate) {
      iemladspa->klass->activate(iemladspa->plugininstance);
    }
  }

	/* Connect controls to the LADSPA Plugin */
  for(i = 0; i < iemladspa->control_data->num_controls; i++) {
    iemladspa->klass->connect_port(iemladspa->plugininstance,
                                 iemladspa->control_data->data[i].index,
                                 &iemladspa->control_data->data[i].data);
		}

  audiobuffer_resize(&iemladspa->inbuf,
                     65536,
                     iemladspa->control_data->sourcechannels.in+iemladspa->control_data->sinkchannels.in);
  audiobuffer_resize(&iemladspa->outbuf,
                     65536,
                     iemladspa->control_data->sourcechannels.out+iemladspa->control_data->sinkchannels.out);

	return 0;
}

/*
 * try to create an alsa ext plugin
 * on success
 *     find an existing iemladspa-plugin with the same root-config
 *     on failure
 *         create a new iemladspa-plugin
 *     (on success)
 *         add ourself to the iemladspa-plugin (and vice-versa 'private_data')
 * on failure
 *   return FAIL
 */

static snd_pcm_iemladspa_t * iemladspa_mergeplugin_create(snd_config_t *sndconfig,
                                                      const char*libname,
                                                      const char*module,
                                                      const char*controlfile,
                                                      iemladspa_iochannels_t sourcechannels, iemladspa_iochannels_t sinkchannels
                                                      ) {
	void *library = NULL;
	const LADSPA_Descriptor *klass=NULL;
	LADSPA_Control *control_data= NULL;
  int success=0;

	/* Open the LADSPA Plugin */
	library = LADSPAload(libname);
	if(library == NULL) goto finalize;

	klass = LADSPAfind(library, libname, module);
	if(klass == NULL)goto finalize;

  control_data = LADSPAcontrolMMAP(klass, controlfile,
                                   sourcechannels, sinkchannels);

  if(NULL == control_data) goto finalize;
  success=1;

 finalize:

  if(success) {
    snd_pcm_iemladspa_t*iemladspa=(snd_pcm_iemladspa_t*)calloc(1, sizeof(snd_pcm_iemladspa_t));
    if(!iemladspa)return NULL;
    iemladspa->sndconfig        = sndconfig;
    iemladspa->library          = library;
    iemladspa->klass            = klass;
    iemladspa->control_data     = control_data;
    iemladspa->stream_direction = -1;

    return iemladspa;
  }
  if(library)
    LADSPAunload(library);
  return NULL;
}


static snd_pcm_iemladspa_t * iemladspa_mergeplugin_findorcreate(snd_config_t *sndconfig,
                                                            const char*libname,
                                                            const char*module,
                                                            const char*controlfile,
                                                            iemladspa_iochannels_t sourcechannels, iemladspa_iochannels_t sinkchannels
                                                            ) {
  /* find a 'iemladspa' instance with 'sndconfig' as sndconfig-configuration */
  snd_pcm_iemladspa_t*iemladspa=linked_list_find(s_mergeplugin_list, sndconfig);
  if(!iemladspa) {
    iemladspa = iemladspa_mergeplugin_create(sndconfig, libname, module, controlfile, sourcechannels, sinkchannels);
    s_mergeplugin_list = linked_list_add(s_mergeplugin_list, sndconfig, iemladspa);
  }
  if(iemladspa)
    iemladspa->usecount++;

  return iemladspa;
}


static snd_pcm_extplug_callback_t iemladspa_callback = {
	.transfer = iemladspa_transfer,
	.init = iemladspa_init,
	.close = iemladspa_close,
};

SND_PCM_PLUGIN_DEFINE_FUNC(iemladspa)
{
	snd_config_iterator_t i, next;
	snd_pcm_iemladspa_t *iemladspa=NULL;
	snd_config_t *sconf = NULL;
	const char *controls = ".alsaiemladspa.bin";
	const char *library = "/usr/lib/ladspa/iemladspa.so";
	const char *module = "iemladspa";
	int err;
  iemladspa_iochannels_t sourcechannels, sinkchannels;
  long inchannels = 2;
  long outchannels = 2;
  unsigned int pcmchannels = 2;
  snd_pcm_extplug_t*ext=NULL;

  const unsigned int supported_formats[] = {SND_PCM_FORMAT_FLOAT, SND_PCM_FORMAT_S16};

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
  sourcechannels.in = sourcechannels.out = inchannels;
  sinkchannels.in   = sinkchannels.out   = outchannels;

	/* Make sure we have a slave and control devices defined */
	if (! sconf) {
		SNDERR("No slave configuration for iemladspa pcm");
		return -EINVAL;
	}

  /* ========= init phase done ============ */

	/* Intialize the local object data */
  iemladspa = iemladspa_mergeplugin_findorcreate(root,
                                             library,
                                             module,
                                             controls,
                                             sourcechannels, sinkchannels);
	if (iemladspa == NULL)
		return -ENOMEM;

  /* check whether we already have an ext for this stream,
     if so, we are done;
     if not, create a new one
  */

  if(SND_PCM_STREAM_PLAYBACK==stream) {
    if(iemladspa->extOUT.private_data == iemladspa)return 0;
    ext=&iemladspa->extOUT;
  } else {
    if(iemladspa->extIN.private_data == iemladspa)return 0;
    ext=&iemladspa->extIN;
  }

	ext->version = SND_PCM_EXTPLUG_VERSION;
	ext->name = "alsaiemladspa";
	ext->callback = &iemladspa_callback;
	ext->private_data = iemladspa;

	/* Create the ALSA External Plugin */
	err = snd_pcm_extplug_create(ext, name, root, sconf, stream, mode);
	if (err < 0) {
    SNDERR("could'nt create extplug '%s'.", name);
		return err;
	}

	/* MMAP to the controls file */
  if(!iemladspa->control_data) {
    iemladspa->control_data = LADSPAcontrolMMAP(iemladspa->klass, controls,
                                              sourcechannels, sinkchannels);
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
  }

	/* Set PCM Contraints */
  pcmchannels = (SND_PCM_STREAM_PLAYBACK == stream)
    ? iemladspa->control_data->sourcechannels.out
    : iemladspa->control_data->sinkchannels.in;

  snd_pcm_extplug_set_param_minmax(ext,
                                   SND_PCM_EXTPLUG_HW_CHANNELS,
                                   pcmchannels,
                                   pcmchannels);

  snd_pcm_extplug_set_slave_param(ext,
                                  SND_PCM_EXTPLUG_HW_CHANNELS,
                                  pcmchannels);

#if 0
	snd_pcm_extplug_set_param(ext,
			SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT);

	snd_pcm_extplug_set_slave_param(ext,
			SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT);
#else
	snd_pcm_extplug_set_param_list(ext, SND_PCM_EXTPLUG_HW_FORMAT,
                                 sizeof(supported_formats)/sizeof(*supported_formats),
                                 supported_formats);
	snd_pcm_extplug_set_slave_param_list(ext, SND_PCM_EXTPLUG_HW_FORMAT,
                                 sizeof(supported_formats)/sizeof(*supported_formats),
                                 supported_formats);
#endif

	*pcmp = ext->pcm;
	
	return 0;

}

SND_PCM_PLUGIN_SYMBOL(iemladspa);

