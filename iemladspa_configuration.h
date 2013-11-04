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

#ifndef IEMLADSPA_CONFIGURATION_H
#define IEMLADSPA_CONFIGURATION_H

#include "ladspa_utils.h"
#include <alsa/asoundlib.h>

typedef enum {
  none = -1,
  PCM,
  CTL
} iemladspa_config_type_t;

typedef struct iemladspa_config_ {
  iemladspa_config_type_t type;
  const char*configname;
  const char*controlfile;
  iemladspa_iochannels_t channels[SND_PCM_STREAM_LAST+1];

  /* LADSPA */
  const char*ladspa_library;
  const char*ladspa_module;

  unsigned int format;
  snd_config_t *slave;
} iemladspa_config_t;

iemladspa_config_t*iemladspa_config_create(iemladspa_config_type_t type);
int iemladspa_config_parse(iemladspa_config_t*conf, snd_config_t *alsaconf);
void iemladspa_config_free(iemladspa_config_t*conf);

#endif /* IEMLADSPA_CONFIGURATION_H */
