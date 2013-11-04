/*
 * alsa-ladspa-bridge: use LADSPA-plugins as ALSA-plugins
 *
 * Copyright (c) 2013 IOhannes m zmölnig - IEM
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

#include "iemladspa_configuration.h"

#include <string.h>

static void iemladspa_config_print(iemladspa_config_t*conf) {
  fprintf(stderr, "conf: %p\n", conf);
  fprintf(stderr, "\ttype=%d\n", conf->type);
  fprintf(stderr, "\tcontrolfile=%s\n", conf->controlfile);
  fprintf(stderr, "\tsourcechannels=%d/%d\n",
          conf->channels[SND_PCM_STREAM_CAPTURE].in,
          conf->channels[SND_PCM_STREAM_CAPTURE].out);
  fprintf(stderr, "\tsinkchannels=%d/%d\n",
          conf->channels[SND_PCM_STREAM_PLAYBACK].in,
          conf->channels[SND_PCM_STREAM_PLAYBACK].out);
  fprintf(stderr, "\tformat=%d\n", conf->format);
  fprintf(stderr, "\tlibrary=%s\n", conf->ladspa_library);
  fprintf(stderr, "\tmodule =%s\n", conf->ladspa_module);
}

static const char*reassign_string(const char*dest, const char*src) {
  if(dest)free((void*)dest);
  dest=NULL;
  if(src) {
    dest=strdup(src);
  }
  return dest;
}

iemladspa_config_t*iemladspa_config_create(iemladspa_config_type_t type) {
  iemladspa_config_t*conf=(iemladspa_config_t*)malloc(sizeof(iemladspa_config_t));

  if(conf) {
    conf->type = type;

    /* fill with default values */
    conf->controlfile=NULL;
    conf->configname =NULL;

    conf->channels[SND_PCM_STREAM_CAPTURE].in   = 2;
    conf->channels[SND_PCM_STREAM_CAPTURE].out  = 2;
    conf->channels[SND_PCM_STREAM_PLAYBACK].in  = 2;
    conf->channels[SND_PCM_STREAM_PLAYBACK].out = 2;

    conf->ladspa_library=strdup("/usr/lib/ladspa/iemladspa.so");
    conf->ladspa_module =strdup("iemladspa");

    conf->slave = NULL;
    conf->format  = SND_PCM_FORMAT_S16;
  }
  return conf;
}

static int iemladspa_subconfig_parse (iemladspa_config_t*CONF, snd_config_t*conf, const char*name, int dir) {
  snd_config_iterator_t i, next;
  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    long channels=0;
    if (snd_config_get_id(n, &id) < 0)
      continue;

    if (strcmp(id, "inchannels") == 0) {
      snd_config_get_integer(n, &channels);
      if(channels < 1) {
        SNDERR("%s.inchannels %d < 1", name, channels);
        return -EINVAL;
      }
      CONF->channels[dir].in =channels;
      continue;
    }

    if (strcmp(id, "outchannels") == 0) {
      snd_config_get_integer(n, &channels);
      if(channels < 1) {
        SNDERR("%s.outchannels %d < 1", name, channels);
        return -EINVAL;
      }
      CONF->channels[dir].out =channels;
      continue;
    }
    SNDERR("unknown config %s.%s", name, id);
    return -EINVAL;
  }
  return 0;

}


void iemladspa_config_free(iemladspa_config_t*conf) {
  if(conf->controlfile)free((void*)conf->controlfile);
  conf->controlfile=NULL;

    conf->channels[SND_PCM_STREAM_CAPTURE].in   = 0;
    conf->channels[SND_PCM_STREAM_CAPTURE].out  = 0;
    conf->channels[SND_PCM_STREAM_PLAYBACK].in  = 0;
    conf->channels[SND_PCM_STREAM_PLAYBACK].out = 0;

  conf->format = SND_PCM_FORMAT_UNKNOWN;

  if(conf->ladspa_library)free((void*)conf->ladspa_library);
  conf->ladspa_library=NULL;

  if(conf->ladspa_module)free((void*)conf->ladspa_module);
  conf->ladspa_module=NULL;

  conf->type = none;
    
  free(conf);
}

int iemladspa_config_parse(iemladspa_config_t*CONF, snd_config_t*conf) {
  snd_config_iterator_t i, next;
  const char*str=NULL;
  char *controls = NULL;
  const char *configname = NULL;

  int pcm=(PCM==CONF->type);

  if (snd_config_get_id(conf, &configname) < 0)
    configname="alsaiemladspa";

  /* Parse configuration options from asoundrc */
  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);

    const char *id;
    if (snd_config_get_id(n, &id) < 0)
      continue;
    if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
      continue;

    if (pcm && (strcmp(id, "slave") == 0)) {
      CONF->slave = n;
      continue;
    }

    if (strcmp(id, "controls") == 0) {
      snd_config_get_string(n, &str);
      controls=(char*)str;
      continue;
    }
    if (strcmp(id, "library") == 0) {
      snd_config_get_string(n, &str);
      CONF->ladspa_library=reassign_string(CONF->ladspa_library, str);
      continue;
    }
    if (strcmp(id, "module") == 0) {
      snd_config_get_string(n, &str);
      CONF->ladspa_module=reassign_string(CONF->ladspa_module, str);
      continue;
    }
    if (pcm && (strcmp(id, "format") == 0)) {
      int format;
      snd_config_get_string(n, &str);
      format=snd_pcm_format_value(str);
      if(SND_PCM_FORMAT_S16!=format && SND_PCM_FORMAT_FLOAT!=format) {
        SNDERR("format must be %s or %s", snd_pcm_format_name(SND_PCM_FORMAT_S16), snd_pcm_format_name(SND_PCM_FORMAT_FLOAT));
        return -EINVAL;
      }
      CONF->format = format;
      continue;
    }
    if (strcmp(id, "inchannels") == 0) {
      long channels = 0;
      snd_config_get_integer(n, &channels);
      if(channels < 1) {
        SNDERR("channels < 1");
        return -EINVAL;
      }
      CONF->channels[SND_PCM_STREAM_CAPTURE].in =channels;
      CONF->channels[SND_PCM_STREAM_CAPTURE].out=channels;
      continue;
    }
    if (strcmp(id, "outchannels") == 0) {
      long channels = 0;
      snd_config_get_integer(n, &channels);
      if(channels < 1) {
        SNDERR("outchannels < 1");
        return -EINVAL;
      }
      CONF->channels[SND_PCM_STREAM_PLAYBACK].in =channels;
      CONF->channels[SND_PCM_STREAM_PLAYBACK].out=channels;
      continue;
    }
    if (strcmp(id, "source") == 0) {
      int er = iemladspa_subconfig_parse(CONF, n, id, SND_PCM_STREAM_CAPTURE);
      if(er)return er;
      continue;
    }
    if (strcmp(id, "sink") == 0) {
      int er = iemladspa_subconfig_parse(CONF, n, id, SND_PCM_STREAM_PLAYBACK);
      if(er)return er;
      continue;
    }
    SNDERR("Unknown field %s", id);
    return -EINVAL;
  }

  if(controls) {
    CONF->controlfile=reassign_string(CONF->controlfile, controls);
  } else if (configname) {
    controls=(char*)calloc(strlen(configname)+5, 1);
    if(!controls) {
      SNDERR("unable to allocate memory for '%s.bin'", configname);
      return -EINVAL;
    }
    sprintf(controls, "%s.bin", configname);
    CONF->controlfile=reassign_string(CONF->controlfile, controls);
    free(controls);controls=NULL;
  }

  if(pcm && NULL == CONF->slave) {
    SNDERR("No slave configuration for pcm.%s", configname);
    return -EINVAL;
  }

  iemladspa_config_print(CONF);
  return 0;
}


