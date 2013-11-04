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

#ifndef IEMLADSPA_UTILS_H
#define IEMLADSPA_UTILS_H

typedef struct _snd_config snd_config_t;
typedef struct snd_pcm_extplug snd_pcm_extplug_t;

void print_pcm_extplug(snd_pcm_extplug_t*ext);
void print_pcm_config(snd_config_t*config, const char*name);

#endif /* IEMLADSPA_UTILS_H */
