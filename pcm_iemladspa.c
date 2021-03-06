/*
 * alsa-ladspa-bridge: use LADSPA-plugins as ALSA-plugins
 *
 * Copyright (c) 2008 Cooper Street Innovations
 * 		<charles@cooper-street.com>
 * Copyright (c) 2013 IOhannes m zm�lnig - IEM
 *		<zmoelnig@iem.at>
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
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

#if 0
# define DEBUG printf("%s:%d %s\t", __FILE__, __LINE__, __FUNCTION__), printf
#else
# define DEBUG(...)
#endif

typedef struct _iemladspa_audiobuf {
  unsigned int frames;
  unsigned int channels;
  size_t size;
  float *data;
} iemladspa_audiobuf_t;

typedef struct _iemladspa_stream {
  iemladspa_audiobuf_t buf;
  iemladspa_audiobuf_t mono;
  snd_pcm_extplug_t    ext;
  int enabled;
} iemladspa_stream_t;

typedef struct snd_pcm_iemladspa {
  iemladspa_stream_t streamdir[SND_PCM_STREAM_LAST+1];
  int stream_direction;

  void *library;
  const LADSPA_Descriptor *klass;
  LADSPA_Control *control_data;
  LADSPA_Handle *plugininstance;

  unsigned int usecount;
  const void   *key;
} snd_pcm_iemladspa_t;

typedef struct linked_list {
  const void*key;
  void*data;
  struct linked_list *next;
} linked_list_t;

void*linked_list_find(linked_list_t*list, const void*key) {
  while(list) {
    if(list->key == key)
      return list->data;
    list=list->next;
  }
  return NULL;
}
linked_list_t*linked_list_add(linked_list_t*list, const void*key, void*data) {
  linked_list_t*entry=(linked_list_t*)calloc(1, sizeof(linked_list_t));
  entry->key=key;
  entry->data=data;
  entry->next=list;

  return entry;
}
linked_list_t*linked_list_delete(linked_list_t*inlist, const void*key) {
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
  printf("SNDCONFIG[%p]: id[%d]=%p: %s\n", config, err, str, str);
  err=snd_config_get_string(config, &str);
  printf("SNDCONFIG[%p]: string[%d]=%p: %s\n", config, err, str, str);
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

/* duplicate the MONO src-channel into <channels> dst-channels */
static inline void samples_duplicate(float*src, float*dst, int frames, int channels) {
  int frame, channel;
  DEBUG("dupe %d/%d\n", frames, channels);
  for(channel=0; channel<channels; channel++) {
    float*out=dst+frames*channel;
    float*in =src;
    for(frame=0; frame<frames; frame++) {
      *out++=*in++;
    }
  }
}
/* mix <channels> src-channels into a MONO dst-channel */
static inline void samples_mixdown(float*src, float*dst, int frames, int channels) {
  int frame, channel;
  float*in,*out;
  DEBUG("mix  %d/%d\n", frames, channels);

  out=dst;
  for(frame=0; frame<frames; frame++) {
    *out++=0.f;
  }

  for(channel=0; channel<channels; channel++) {
    in=src+frames*channel;
    out=dst;
    for(frame=0; frame<frames; frame++) {
      *out++ += *in++;
    }
  }
}

/* mute <channels> int <dst> */
static inline void samples_mute(float*dst, int frames, int channels) {
  int frame, channel;
  DEBUG("mute %d/%d\n", frames, channels);
  for(channel=0; channel<channels; channel++) {
    float*out=dst+frames*channel;
    for(frame=0; frame<frames; frame++) {
      *out++=0.f;
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
  DEBUG("transfer: stream=%d\tchannels=%d\tslavechannels=%d\n", ext->stream, ext->channels, ext->slave_channels);

  snd_pcm_iemladspa_t *iemladspa = (snd_pcm_iemladspa_t *)(ext->private_data);
  const int playback = (SND_PCM_STREAM_PLAYBACK == ext->stream);

  /* input/output channels for the alsa-plugin (transfer call) */
  const unsigned int alsa_inchannels  = ( playback)?ext->channels:ext->slave_channels;
  const unsigned int alsa_outchannels = (!playback)?ext->channels:ext->slave_channels;

  /* input/output channels for the ladspa-plugin */
  const unsigned int inchannels  = (playback)?(iemladspa->control_data->sourcechannels.in ):(iemladspa->control_data->sinkchannels.in);
  const unsigned int outchannels = (playback)?(iemladspa->control_data->sourcechannels.out):(iemladspa->control_data->sinkchannels.out);

  /* offset in samples to jump to the correct channel in the de-interleaved data */
  const unsigned long bufoffset_in_src  = 0;
  const unsigned long bufoffset_in_snk  = (iemladspa->control_data->sourcechannels.in  * size);
  const unsigned long bufoffset_out_src = 0;
  const unsigned long bufoffset_out_snk = (iemladspa->control_data->sourcechannels.out * size);

  const unsigned long bufoffset_in  = (playback)? bufoffset_in_snk :bufoffset_in_src;
  const unsigned long bufoffset_out = (playback)? bufoffset_out_snk:bufoffset_out_src;

  /* LADSPA-port offset (to skip control-ports in port_connect */
  const unsigned long dataoffset_in  = iemladspa->control_data->num_controls;
  const unsigned long dataoffset_out = dataoffset_in + iemladspa->control_data->num_inchannels;

  int j;

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
  audiobuffer_resize(&iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].buf , size,
                     iemladspa->control_data->sourcechannels.in +iemladspa->control_data->sinkchannels.in);
  audiobuffer_resize(&iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].buf, size,
                     iemladspa->control_data->sourcechannels.out+iemladspa->control_data->sinkchannels.out);

  /* NOTE: swap source and destination memory space when deinterleaved.
     then swap it back during the interleave call below */

  if(!playback | (alsa_inchannels == inchannels)) {
    /* in CAPTURE mode, we always have the correct number of input channels;
     * deinterleave the data into *channels.in channels
     */
    deinterleave(src,
                 iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].buf.data + bufoffset_in,
                 size, inchannels);
  } else if (1==alsa_inchannels) {
    /* if we are in PLAYBACK mode, the client-application might send us data in MONO;
     * deinterleave the data into a MONO buffer, then blow it up to *channels.in
     */
    audiobuffer_resize(&iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].mono, size, 1);
 
    /* "deinterleave" */
    deinterleave(src,
                 iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].mono.data,
                 size, 1);

    /* dupe */
    samples_duplicate(iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].mono.data,
                      iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].buf.data + bufoffset_in,
                      size, inchannels);
  } else {
    // this should never happen
  }


  /* only run when
   *   - stream is in playback mode (if we are opened with PLAYBACK)
   *   - stream is in capture mode (if we don't have PLAYBACK)
   */
  if((playback) || (!iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].enabled)) {
    /* MUTE all unused input channels */
    if(!iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].enabled) {
      samples_mute(iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].buf.data + bufoffset_in_src,
		   size, iemladspa->control_data->sourcechannels.in);
    }
    if(!iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].enabled) {
      samples_mute(iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].buf.data + bufoffset_in_snk,
		   size, iemladspa->control_data->sourcechannels.in);
    }

    for(j = 0; j < iemladspa->control_data->num_inchannels; j++) {
      connect_port(iemladspa,
                   iemladspa->control_data->data[dataoffset_in + j].index,
                   iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].buf.data + j*size,
                   "inport "
                   );
    }
    for(j = 0; j < iemladspa->control_data->num_outchannels; j++) {
      connect_port(iemladspa,
                   iemladspa->control_data->data[dataoffset_out+ j].index,
                   iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].buf.data + j*size,
                   "outport");
    }

    iemladspa->klass->run(iemladspa->plugininstance, size);
  }

  /* MUTE all unused input channels */
  if(!iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].enabled) {
    samples_mute(iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].buf.data + bufoffset_out_src,
		 size, iemladspa->control_data->sourcechannels.out);
  }
  if(!iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].enabled) {
    samples_mute(iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].buf.data + bufoffset_out_snk,
		 size, iemladspa->control_data->sourcechannels.out);
  }

  if(playback | (alsa_outchannels == outchannels)) {
    /* in PLAYBACK mode, we always have the correct number of output channels;
     * interleave the data into sinkchannels.out channels
     */
    reinterleave(iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].buf.data + bufoffset_out,
                 dst,
                 size, outchannels);
  } else if (1==alsa_outchannels) {
    /* if we are in CAPTURE mode, the client-application might receive data in MONO;
     * mix the data into *channels.out, then interleave it into a MONO buffer
     */
    audiobuffer_resize(&iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].mono, size, 1);

    /* mixdown */
    samples_mixdown(iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].buf.data + bufoffset_out,
                    iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].mono.data,
                    size, outchannels);

    /* "reinterleave" */
    reinterleave(iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].mono.data,
                 dst,
                 size, 1);
  } else {
    // this should never happen
  }
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

  s_mergeplugin_list = linked_list_delete(s_mergeplugin_list, iemladspa->key);
  free(iemladspa);
  return 0;
}

static int iemladspa_init(snd_pcm_extplug_t *ext)
{
  const unsigned int default_frames=65536;
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

  audiobuffer_resize(&iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].buf,
                     default_frames,
                     iemladspa->control_data->sourcechannels.in+iemladspa->control_data->sinkchannels.in);
  audiobuffer_resize(&iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].buf,
                     default_frames,
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

static snd_pcm_iemladspa_t * iemladspa_mergeplugin_create(const void *key,
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
    iemladspa->key              = key;
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


static snd_pcm_iemladspa_t * iemladspa_mergeplugin_findorcreate(const void *key,
                                                                const char*libname,
                                                                const char*module,
                                                                const char*controlfile,
                                                                iemladspa_iochannels_t sourcechannels, iemladspa_iochannels_t sinkchannels
                                                                ) {
  /* find a 'iemladspa' instance with 'key' */
  snd_pcm_iemladspa_t*iemladspa=linked_list_find(s_mergeplugin_list, key);
  if(!iemladspa) {
    iemladspa = iemladspa_mergeplugin_create(key, libname, module, controlfile, sourcechannels, sinkchannels);
    s_mergeplugin_list = linked_list_add(s_mergeplugin_list, key, iemladspa);
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
  const char *controls = NULL;
  char *default_controls=NULL;
  const char *library = "/usr/lib/ladspa/iemladspa.so";
  const char *module = "iemladspa";
  int err;
  iemladspa_iochannels_t sourcechannels, sinkchannels;
  long inchannels = 2;
  long outchannels = 2;
  unsigned int pcmchannels = 2;
  snd_pcm_extplug_t*ext=NULL;
  const char *configname = NULL;

  unsigned int format = SND_PCM_FORMAT_S16;

  if (snd_config_get_id(conf, &configname) < 0)
    configname=NULL;

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
    if (strcmp(id, "format") == 0) {
      const char*fmt=NULL;
      snd_config_get_string(n, &fmt);
      format=snd_pcm_format_value(fmt);
      if(SND_PCM_FORMAT_S16!=format && SND_PCM_FORMAT_FLOAT!=format) {
        SNDERR("format must be %s or %s", snd_pcm_format_name(SND_PCM_FORMAT_S16), snd_pcm_format_name(SND_PCM_FORMAT_FLOAT));
        return -EINVAL;

      }
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

  if(!controls) {
    default_controls=(char*)calloc(strlen(configname)+5, 1);
    if(!default_controls) {
      SNDERR("unable to allocate memory for '%s.bin'", configname);
      return -EINVAL;
    }
    sprintf(default_controls, "%s.bin", configname);
    controls=default_controls;
  }

  /* ========= init phase done ============ */

  /* Intialize the local object data */
  iemladspa = iemladspa_mergeplugin_findorcreate(configname,
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
    iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].enabled=1;
    if(iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].ext.private_data == iemladspa)return 0;
    ext=&iemladspa->streamdir[SND_PCM_STREAM_PLAYBACK].ext;
  } else {
    iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].enabled=1;
    if(iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].ext.private_data == iemladspa)return 0;
    ext=&iemladspa->streamdir[SND_PCM_STREAM_CAPTURE].ext;
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

  /* MONO support: we really should make an enumeration, rather than minmax */
#if 0
  snd_pcm_extplug_set_param_minmax(ext,
                                   SND_PCM_EXTPLUG_HW_CHANNELS,
                                   1, /* allow opending MONO */
                                   pcmchannels);
#else
  DEBUG("dir=%d\tpcmchannels=%d\n", stream, pcmchannels);
  if(1==pcmchannels) {
    snd_pcm_extplug_set_param(ext,
                              SND_PCM_EXTPLUG_HW_CHANNELS,
                              pcmchannels);
  } else {
    unsigned int list [2];
    list[1]=1;
    list[0]=pcmchannels;
    snd_pcm_extplug_set_param_list(ext,
                                   SND_PCM_EXTPLUG_HW_CHANNELS,
                                   2, list);
  }
#endif

  snd_pcm_extplug_set_slave_param_minmax(ext,
                                  SND_PCM_EXTPLUG_HW_CHANNELS,
                                         pcmchannels,
                                         pcmchannels);

  snd_pcm_extplug_set_param(ext, SND_PCM_EXTPLUG_HW_FORMAT, format);
  snd_pcm_extplug_set_slave_param(ext, SND_PCM_EXTPLUG_HW_FORMAT, format);

  *pcmp = ext->pcm;

  if(default_controls)
    free(default_controls);

  return 0;
}

SND_PCM_PLUGIN_SYMBOL(iemladspa);

