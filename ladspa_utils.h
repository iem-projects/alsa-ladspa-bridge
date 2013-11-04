/* utils.h

   Free software by Richard W.E. Furse. Do with as you will. No
   warranty. */

#ifndef LADSPA_SDK_LOAD_PLUGIN_LIB
#define LADSPA_SDK_LOAD_PLUGIN_LIB

#include <ladspa.h>
#include "iemladspa_configuration.h"

/* This function call takes a plugin library filename, searches for
   the library along the LADSPA_PATH, loads it with dlopen() and
   returns a plugin handle for use with findPluginDescriptor() or
   unloadLADSPAPluginLibrary(). Errors are handled by writing a
   message to stderr and calling exit(1). It is alright (although
   inefficient) to call this more than once for the same file. */
void * LADSPAload(const char * pcPluginFilename);

/* This function unloads a LADSPA plugin library. */
void LADSPAunload(void * pvLADSPAPluginLibrary);

/* This function locates a LADSPA plugin within a plugin library
   loaded with loadLADSPAPluginLibrary(). Errors are handled by
   writing a message to stderr and calling exit(1). Note that the
   plugin library filename is only included to help provide
   informative error messages. */
const LADSPA_Descriptor *
LADSPAfind(void * pvLADSPAPluginLibrary,
           const char * pcPluginLibraryFilename,
           const char * pcPluginLabel);

/* Find the default value for a port. Return 0 if a default is found
   and -1 if not. */
int LADSPADefault(const LADSPA_PortRangeHint * psPortRangeHint,
                  const unsigned long          lSampleRate,
                  LADSPA_Data                * pfResult);

/* MMAP to a controls file */
#define LADSPA_CNTRL_INPUT	0
#define LADSPA_CNTRL_OUTPUT	1
typedef struct LADSPA_Control_Data_ {
  int index;
  LADSPA_Data data;
  int type;
} LADSPA_Control_Data;
typedef struct LADSPA_Control_ {
  unsigned long length;
  unsigned long id;
  iemladspa_iochannels_t sourcechannels; /* source device (e.g. microphone) channels */
  iemladspa_iochannels_t sinkchannels;   /* sink device (e.g. speaker) channels */

  unsigned long num_controls;    /* number of controls in ladspa-plugin (input only?) */

  unsigned long num_inchannels;  /* number of input ports in ladspa-plugin */  // DEPRECATED, use sourcechannels.in+sinkchannels.in
  unsigned long num_outchannels; /* number of output ports in ladspa-plugin */ // DEPRECATED, use sourcechannels.out+sinkchannels.out

  LADSPA_Control_Data data[]; /* controls, inchannels, outchannels */
} LADSPA_Control;
LADSPA_Control * LADSPAcontrolMMAP(const LADSPA_Descriptor *psDescriptor,
                                   const char *controls_filename,
                                   iemladspa_iochannels_t sourcechannels, iemladspa_iochannels_t sinkchannels);
void LADSPAcontrolUnMMAP(LADSPA_Control *control);

#endif
