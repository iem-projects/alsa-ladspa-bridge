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

#include "iemladspa_utils.h"
#include <stdio.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_extplug.h>

void print_pcm_extplug(snd_pcm_extplug_t*ext) {
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

void print_pcm_config(snd_config_t*config, const char*name) {
  int err;
  const char*str;

  printf("SNDCONFIG[%p]: '%s'=[%d]\n", config, name, snd_config_get_type(config));
  err=snd_config_get_id(config, &str);
  printf("SNDCONFIG[%p]: id[%d]=%p: %s\n", config, err, str, str);
  err=snd_config_get_string(config, &str);
  printf("SNDCONFIG[%p]: string[%d]=%p: %s\n", config, err, str, str);
  printf("\n");
}
