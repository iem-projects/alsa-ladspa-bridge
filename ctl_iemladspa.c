/*
 * alsa-ladspa-bridge: use LADSPA-plugins as ALSA-plugins
 *
 * Copyright (c) 2008 Cooper Street Innovations
 * 		<charles@cooper-street.com>
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

#include <stdio.h>
#include <alsa/asoundlib.h>
#include <alsa/control_external.h>

#include <ladspa.h>
#include "ladspa_utils.h"

typedef struct snd_ctl_iemladspa_control {
  long min;
  long max;
  char *name;
} snd_ctl_iemladspa_control_t;

typedef struct snd_ctl_iemladspa {
  snd_ctl_ext_t ext;
  void *library;
  const LADSPA_Descriptor *klass;
  int num_input_controls;
  LADSPA_Control *control_data;
  snd_ctl_iemladspa_control_t *control_info;
} snd_ctl_iemladspa_t;

static void iemladspa_close(snd_ctl_ext_t *ext)
{
  snd_ctl_iemladspa_t *iemladspa = ext->private_data;
  int i;
  for (i = 0; i < iemladspa->num_input_controls; i++) {
    free(iemladspa->control_info[i].name);
  }
  free(iemladspa->control_info);
  LADSPAcontrolUnMMAP(iemladspa->control_data);
  LADSPAunload(iemladspa->library);
  free(iemladspa);
}

static int iemladspa_elem_count(snd_ctl_ext_t *ext)
{
  snd_ctl_iemladspa_t *iemladspa = ext->private_data;
  return iemladspa->num_input_controls;
}

static int iemladspa_elem_list(snd_ctl_ext_t *ext, unsigned int offset,
                               snd_ctl_elem_id_t *id)
{
  snd_ctl_iemladspa_t *iemladspa = ext->private_data;
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_id_set_name(id, iemladspa->control_info[offset].name);
  snd_ctl_elem_id_set_device(id, offset);
  return 0;
}

static snd_ctl_ext_key_t iemladspa_find_elem(snd_ctl_ext_t *ext,
                                             const snd_ctl_elem_id_t *id)
{
  snd_ctl_iemladspa_t *iemladspa = ext->private_data;
  const char *name;
  unsigned int i, key;

  name = snd_ctl_elem_id_get_name(id);

  for (i = 0; i < iemladspa->num_input_controls; i++) {
    key = i;
    if (!strcmp(name, iemladspa->control_info[key].name)) {
      return key;
    }
  }

  return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int iemladspa_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                                   int *type, unsigned int *acc, unsigned int *count)
{
  *type = SND_CTL_ELEM_TYPE_INTEGER;
  *acc = SND_CTL_EXT_ACCESS_READWRITE;
  *count = 1;
  return 0;
}

static int iemladspa_get_integer_info(snd_ctl_ext_t *ext,
                                      snd_ctl_ext_key_t key, long *imin, long *imax, long *istep)
{
  *istep = 1;
  *imin = 0;
  *imax = 100;
  return 0;
}

/* read data from ladspa-plugin */
static int iemladspa_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                                  long *value)
{
  snd_ctl_iemladspa_t *iemladspa = ext->private_data;
  LADSPA_Data v = iemladspa->control_data->data[key].data;

  if (iemladspa->control_info[key].max == iemladspa->control_info[key].min) {
    value[0]= v * 100;
  } else {
    value[0] = ((v - iemladspa->control_info[key].min)/
                (iemladspa->control_info[key].max-
                 iemladspa->control_info[key].min))*100;
  }

  return sizeof(long);
}

/* write data to ladspa-plugin */
static int iemladspa_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                                   long *value)
{
  snd_ctl_iemladspa_t *iemladspa = ext->private_data;
  float setting;

  setting = value[0];
  if (iemladspa->control_info[key].max == iemladspa->control_info[key].min) {
    iemladspa->control_data->data[key].data = (setting/100);
  } else {
    iemladspa->control_data->data[key].data = (setting/100)*
      (iemladspa->control_info[key].max-
       iemladspa->control_info[key].min)+
      iemladspa->control_info[key].min;
  }

  return 1;
}

static int iemladspa_read_event(snd_ctl_ext_t *ext ATTRIBUTE_UNUSED,
                                snd_ctl_elem_id_t *id ATTRIBUTE_UNUSED,
                                unsigned int *event_mask ATTRIBUTE_UNUSED)
{
  return -EAGAIN;
}

static snd_ctl_ext_callback_t iemladspa_ext_callback = {
  .close = iemladspa_close,
  .elem_count = iemladspa_elem_count,
  .elem_list = iemladspa_elem_list,
  .find_elem = iemladspa_find_elem,
  .get_attribute = iemladspa_get_attribute,
  .get_integer_info = iemladspa_get_integer_info,
  .read_integer = iemladspa_read_integer,
  .write_integer = iemladspa_write_integer,
  .read_event = iemladspa_read_event,
};

SND_CTL_PLUGIN_DEFINE_FUNC(iemladspa)
{
  /* TODO: Plug all of the memory leaks if these some initialization
     failure */
  snd_config_iterator_t it, next;
  snd_ctl_iemladspa_t *iemladspa;
  const char *controls = ".alsaiemladspa.bin";
  const char *library = "/usr/lib/ladspa/iemladspa.so";
  const char *module = "iemladspa";
  int err, i, index;

  iemladspa_iochannels_t sourcechannels, sinkchannels;
  long inchannels = 2;
  long outchannels = 2;

  /* Parse configuration options from asoundrc */
  snd_config_for_each(it, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(it);
    const char *id;
    if (snd_config_get_id(n, &id) < 0)
      continue;
    if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
      continue;
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

  /* Intialize the local object data */
  iemladspa = calloc(1, sizeof(*iemladspa));
  if (iemladspa == NULL)
    return -ENOMEM;

  iemladspa->ext.version = SND_CTL_EXT_VERSION;
  iemladspa->ext.card_idx = 0;
  iemladspa->ext.poll_fd = -1;
  iemladspa->ext.callback = &iemladspa_ext_callback;
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

  /* Import data from the LADSPA Plugin */
  strncpy(iemladspa->ext.id, iemladspa->klass->Label, sizeof(iemladspa->ext.id));
  strncpy(iemladspa->ext.driver, "LADSPA Plugin", sizeof(iemladspa->ext.driver));
  strncpy(iemladspa->ext.name, iemladspa->klass->Label, sizeof(iemladspa->ext.name));
  strncpy(iemladspa->ext.longname, iemladspa->klass->Name,
          sizeof(iemladspa->ext.longname));
  strncpy(iemladspa->ext.mixername, "LADSPA ALSA", sizeof(iemladspa->ext.mixername));

  /* Create the ALSA External Plugin */
  err = snd_ctl_ext_create(&iemladspa->ext, name, SND_CTL_NONBLOCK);
  if (err < 0) {
    return -1;
  }

  /* MMAP to the controls file */
  iemladspa->control_data = LADSPAcontrolMMAP(iemladspa->klass, controls,
                                              sourcechannels, sinkchannels);
  if(iemladspa->control_data == NULL) {
    return -1;
  }
	
  iemladspa->num_input_controls = 0;
  for(i = 0; i < iemladspa->control_data->num_controls; i++) {
    if(iemladspa->control_data->data[i].type == LADSPA_CNTRL_INPUT) {
      iemladspa->num_input_controls++;
    }
  }
	
  /* Pull in data from controls file */
  iemladspa->control_info = malloc(
                                   sizeof(snd_ctl_iemladspa_control_t)*iemladspa->num_input_controls);
  if(iemladspa->control_info == NULL) {
    return -1;
  }

  for(i = 0; i < iemladspa->num_input_controls; i++) {
    if(iemladspa->control_data->data[i].type == LADSPA_CNTRL_INPUT) {
      index = iemladspa->control_data->data[i].index;
      if(index>=iemladspa->klass->PortCount || iemladspa->klass->PortDescriptors[index] !=
         (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL)) {
	SNDERR("Problem with control file %s, %d.", controls, index);
	return -1;
      }
      iemladspa->control_info[i].min =
        iemladspa->klass->PortRangeHints[index].LowerBound;
      iemladspa->control_info[i].max =
        iemladspa->klass->PortRangeHints[index].UpperBound;

      iemladspa->control_info[i].name = strdup(iemladspa->klass->PortNames[index]);
      if(iemladspa->control_info[i].name == NULL) {
	return -1;
      }
    }
  }

  /* Make sure that the control file makes sense */
  const unsigned long offset_in = iemladspa->control_data->num_controls;
  const unsigned long offset_out = offset_in + iemladspa->control_data->num_inchannels;
  for(i=0; i<iemladspa->control_data->num_inchannels; i++) {
    unsigned int index=iemladspa->control_data->data[offset_in + i].index;
    if(index>=iemladspa->klass->PortCount || iemladspa->klass->PortDescriptors[index] !=
       (LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO)) {
      SNDERR("Problem with control file %s.", controls);
      return -1;
    }
  }
  for(i=0; i<iemladspa->control_data->num_outchannels; i++) {
    unsigned int index=iemladspa->control_data->data[offset_out + i].index;

    if(index>=iemladspa->klass->PortCount || iemladspa->klass->PortDescriptors[index] !=
       (LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO)) {
      SNDERR("Problem with control file %s.", controls);
      return -1;
    }
  }

  *handlep = iemladspa->ext.handle;
  return 0;

}

SND_CTL_PLUGIN_SYMBOL(iemladspa);
