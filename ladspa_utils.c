/* ------------------------------------------------------------------
   Free software by Richard W.E. Furse. Do with as you will. No
   warranty.
  ------------------------------------------------------------------*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <string.h>
#include <math.h>

#include <sys/stat.h>

#include <ladspa.h>
#include "ladspa_utils.h"


/* the following do_mkdir() and mkpath() implementation is (c) 2009 Jonathan Leffler
 * under follwing license:
 *  You are hereby given permission to use this code for any purpose with attribution.
 */
typedef struct stat Stat;
static int do_mkdir(const char *path, mode_t mode)
{
    Stat            st;
    int             status = 0;

    if (stat(path, &st) != 0)
    {
        /* Directory does not exist. EEXIST for race condition */
        if (mkdir(path, mode) != 0 && errno != EEXIST)
            status = -1;
    }
    else if (!S_ISDIR(st.st_mode))
    {
        errno = ENOTDIR;
        status = -1;
    }

    return(status);
}

/**
** mkpath - ensure all directories in path exist
** Algorithm takes the pessimistic view and works top-down to ensure
** each directory in path exists, rather than optimistically creating
** the last element and working backwards.
*/
int mkpath(const char *path, mode_t mode)
{
    char           *pp;
    char           *sp;
    int             status;
    char           *copypath = strdup(path);

    status = 0;
    pp = copypath;
    while (status == 0 && (sp = strchr(pp, '/')) != 0)
    {
        if (sp != pp)
        {
            /* Neither root nor double slash in path */
            *sp = '\0';
            status = do_mkdir(copypath, mode);
            *sp = '/';
        }
        pp = sp + 1;
    }
    if (status == 0)
        status = do_mkdir(path, mode);
    free(copypath);
    return (status);
}

/* ------------------------------------------------------------------ */

/* This function provides a wrapping of dlopen(). When the filename is
   not an absolute path (i.e. does not begin with / character), this
   routine will search the LADSPA_PATH for the file. */
static void *
dlopenLADSPA(const char * pcFilename, int iFlag) {

  char * pcBuffer;
  const char * pcEnd;
  const char * pcLADSPAPath;
  const char * pcStart;
  int iEndsInSO;
  int iNeedSlash;
  size_t iFilenameLength;
  void * pvResult;

  iFilenameLength = strlen(pcFilename);
  pvResult = NULL;

  if (pcFilename[0] == '/') {

    /* The filename is absolute. Assume the user knows what he/she is
       doing and simply dlopen() it. */

    pvResult = dlopen(pcFilename, iFlag);
    if (pvResult != NULL)
      return pvResult;

  }
  else {

    /* If the filename is not absolute then we wish to check along the
       LADSPA_PATH path to see if we can find the file there. We do
       NOT call dlopen() directly as this would find plugins on the
       LD_LIBRARY_PATH, whereas the LADSPA_PATH is the correct place
       to search. */

    pcLADSPAPath = getenv("LADSPA_PATH");

    if (pcLADSPAPath) {

      pcStart = pcLADSPAPath;
      while (*pcStart != '\0') {
	pcEnd = pcStart;
	while (*pcEnd != ':' && *pcEnd != '\0')
	  pcEnd++;

	pcBuffer = malloc(iFilenameLength + 2 + (pcEnd - pcStart));
	if (pcEnd > pcStart)
	  strncpy(pcBuffer, pcStart, pcEnd - pcStart);
	iNeedSlash = 0;
	if (pcEnd > pcStart)
	  if (*(pcEnd - 1) != '/') {
	    iNeedSlash = 1;
	    pcBuffer[pcEnd - pcStart] = '/';
	  }
	strcpy(pcBuffer + iNeedSlash + (pcEnd - pcStart), pcFilename);

	pvResult = dlopen(pcBuffer, iFlag);

	free(pcBuffer);
	if (pvResult != NULL)
	  return pvResult;

	pcStart = pcEnd;
	if (*pcStart == ':')
	  pcStart++;
      }
    }
  }

  /* As a last ditch effort, check if filename does not end with
     ".so". In this case, add this suffix and recurse. */
  iEndsInSO = 0;
  if (iFilenameLength > 3)
    iEndsInSO = (strcmp(pcFilename + iFilenameLength - 3, ".so") == 0);
  if (!iEndsInSO) {
    pcBuffer = malloc(iFilenameLength + 4);
    strcpy(pcBuffer, pcFilename);
    strcat(pcBuffer, ".so");
    pvResult = dlopenLADSPA(pcBuffer, iFlag);
    free(pcBuffer);
  }

  if (pvResult != NULL)
    return pvResult;

  /* If nothing has worked, then at least we can make sure we set the
     correct error message - and this should correspond to a call to
     dlopen() with the actual filename requested. The dlopen() manual
     page does not specify whether the first or last error message
     will be kept when multiple calls are made to dlopen(). We've
     covered the former case - now we can handle the latter by calling
     dlopen() again here. */
  return dlopen(pcFilename, iFlag);
}

/* ------------------------------------------------------------------ */

void * LADSPAload(const char * pcPluginFilename) {

  void * pvPluginHandle;

  pvPluginHandle = dlopenLADSPA(pcPluginFilename, RTLD_NOW);
  if (!pvPluginHandle) {
    fprintf(stderr,
	    "Failed to load plugin \"%s\": %s\n",
	    pcPluginFilename,
	    dlerror());
    exit(1);
  }

  return pvPluginHandle;
}


void LADSPAunload(void * pvLADSPAPluginLibrary) {
  dlclose(pvLADSPAPluginLibrary);
}

const LADSPA_Descriptor * LADSPAfind(void * pvLADSPAPluginLibrary,
			   const char * pcPluginLibraryFilename,
			   const char * pcPluginLabel) {

  const LADSPA_Descriptor * psDescriptor;
  LADSPA_Descriptor_Function pfDescriptorFunction;
  unsigned long lPluginIndex;

  dlerror();
  pfDescriptorFunction
    = (LADSPA_Descriptor_Function)dlsym(pvLADSPAPluginLibrary,
					"ladspa_descriptor");
  if (!pfDescriptorFunction) {
    const char * pcError = dlerror();
    if (pcError) {
      fprintf(stderr,
	      "Unable to find ladspa_descriptor() function in plugin "
	      "library file \"%s\": %s.\n"
	      "Are you sure this is a LADSPA plugin file?\n",
	      pcPluginLibraryFilename,
	      pcError);
      exit(1);
    }
  }

  for (lPluginIndex = 0;; lPluginIndex++) {
    psDescriptor = pfDescriptorFunction(lPluginIndex);
    if (psDescriptor == NULL) {
      fprintf(stderr,
	      "Unable to find label \"%s\" in plugin library file \"%s\".\n",
	      pcPluginLabel,
	      pcPluginLibraryFilename);
      exit(1);
    }
    if (strcmp(psDescriptor->Label, pcPluginLabel) == 0)
      return psDescriptor;
  }
}

/* ------------------------------------------------------------------ */

int LADSPADefault(const LADSPA_PortRangeHint * psPortRangeHint,
		 const unsigned long          lSampleRate,
		 LADSPA_Data                * pfResult) {

  int iHintDescriptor;

  iHintDescriptor = psPortRangeHint->HintDescriptor & LADSPA_HINT_DEFAULT_MASK;

  switch (iHintDescriptor & LADSPA_HINT_DEFAULT_MASK) {
  case LADSPA_HINT_DEFAULT_NONE:
    return -1;
  case LADSPA_HINT_DEFAULT_MINIMUM:
    *pfResult = psPortRangeHint->LowerBound;
    if (LADSPA_IS_HINT_SAMPLE_RATE(psPortRangeHint->HintDescriptor))
      *pfResult *= lSampleRate;
    return 0;
  case LADSPA_HINT_DEFAULT_LOW:
    if (LADSPA_IS_HINT_LOGARITHMIC(iHintDescriptor)) {
      *pfResult = exp(log(psPortRangeHint->LowerBound) * 0.75
		      + log(psPortRangeHint->UpperBound) * 0.25);
    }
    else {
      *pfResult = (psPortRangeHint->LowerBound * 0.75
		   + psPortRangeHint->UpperBound * 0.25);
    }
    if (LADSPA_IS_HINT_SAMPLE_RATE(psPortRangeHint->HintDescriptor))
      *pfResult *= lSampleRate;
    return 0;
  case LADSPA_HINT_DEFAULT_MIDDLE:
    if (LADSPA_IS_HINT_LOGARITHMIC(iHintDescriptor)) {
      *pfResult = sqrt(psPortRangeHint->LowerBound
		       * psPortRangeHint->UpperBound);
    }
    else {
      *pfResult = 0.5 * (psPortRangeHint->LowerBound
			 + psPortRangeHint->UpperBound);
    }
    if (LADSPA_IS_HINT_SAMPLE_RATE(psPortRangeHint->HintDescriptor))
      *pfResult *= lSampleRate;
    return 0;
  case LADSPA_HINT_DEFAULT_HIGH:
    if (LADSPA_IS_HINT_LOGARITHMIC(iHintDescriptor)) {
      *pfResult = exp(log(psPortRangeHint->LowerBound) * 0.25
		      + log(psPortRangeHint->UpperBound) * 0.75);
    }
    else {
      *pfResult = (psPortRangeHint->LowerBound * 0.25
		   + psPortRangeHint->UpperBound * 0.75);
    }
    if (LADSPA_IS_HINT_SAMPLE_RATE(psPortRangeHint->HintDescriptor))
      *pfResult *= lSampleRate;
    return 0;
  case LADSPA_HINT_DEFAULT_MAXIMUM:
    *pfResult = psPortRangeHint->UpperBound;
    if (LADSPA_IS_HINT_SAMPLE_RATE(psPortRangeHint->HintDescriptor))
      *pfResult *= lSampleRate;
    return 0;
  case LADSPA_HINT_DEFAULT_0:
    *pfResult = 0;
    return 0;
  case LADSPA_HINT_DEFAULT_1:
    *pfResult = 1;
    return 0;
  case LADSPA_HINT_DEFAULT_100:
    *pfResult = 100;
    return 0;
  case LADSPA_HINT_DEFAULT_440:
    *pfResult = 440;
    return 0;
  }

  /* We don't recognise this default flag. It's probably from a more
     recent version of LADSPA. */
  return -1;
}

/* ------------------------------------------------------------------ */

void LADSPAcontrolUnMMAP(LADSPA_Control *control)
{
	munmap(control, control->length);
}

LADSPA_Control * LADSPAcontrolMMAP(const LADSPA_Descriptor *psDescriptor,
                                   const char *controls_filename,
                                   iemladspa_iochannels_t sourcechannels, iemladspa_iochannels_t sinkchannels)
{
	const char * homePath;
	char *filename;
	unsigned long i, index, iindex, oindex;
  unsigned int num_controls = 0, num_inchannels = 0, num_outchannels = 0;

	LADSPA_Control *default_controls;
	LADSPA_Control *ptr;
	int fd;
	unsigned long length;

	/* Create config filename, if no path specified store in home directory */
	if (controls_filename[0] == '/') {
		filename = malloc(strlen(controls_filename) + 1);
		if (filename==NULL) {
			return NULL;
		}
		sprintf(filename, "%s", controls_filename);
	} else {
    const char*subdir="/.config/alsa.iem.at/";
		homePath = getenv("HOME");
		if (homePath==NULL) {
			return NULL;
		}
		filename = malloc(strlen(controls_filename) + strlen(subdir) + strlen(homePath) + 1);
		if (filename==NULL) {
			return NULL;
		}
		sprintf(filename, "%s%s", homePath, subdir);
    if(mkpath(filename, 0770)) {
      return NULL;
    }

		sprintf(filename, "%s%s%s", homePath, subdir, controls_filename);
	}

	/* Count the number of controls */
	num_controls = 0;
  num_inchannels=0;
  num_outchannels=0;

  for(i = 0; i < psDescriptor->PortCount; i++) {
    if(psDescriptor->PortDescriptors[i]&LADSPA_PORT_CONTROL)
      num_controls++;
    else if(psDescriptor->PortDescriptors[i] == (LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO))
      num_inchannels++;
    else if(psDescriptor->PortDescriptors[i] == (LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO))
      num_outchannels++;
  }

	if(num_controls == 0) {
		fprintf(stderr, "No Controls on LADSPA Module.\n");
		return NULL;
	}

  if(num_inchannels != (sourcechannels.in  + sinkchannels.in)) {
		fprintf(stderr, "LADSPA Module has %d channels but we need %d+%d.\n", num_inchannels, sourcechannels.in, sinkchannels.in);
		return NULL;
  }

	/* Calculate the required file-size */
	length =                                          sizeof(LADSPA_Control)
    + (num_inchannels+num_outchannels+num_controls)*sizeof(LADSPA_Control_Data)
    ;

	/* Open config file */
	fd = open(filename, O_RDWR);
	if(fd < 0) {
		if(errno == ENOENT){
			/* If the file doesn't exist create it and populate
				it with default data. */
			fd = open(filename, O_RDWR | O_CREAT, 0664);
			if(fd < 0) {
				fprintf(stderr, "Failed to open controls file:%s.\n",
						filename);
				free(filename);
				return NULL;
			}
			/* Create default controls stucture */
			default_controls = malloc(length);
			if(default_controls == NULL) {
				free(filename);
				return NULL;
			}
			default_controls->length = length;
			default_controls->id = psDescriptor->UniqueID;

      default_controls->sourcechannels.in  = sourcechannels.in;
      default_controls->sourcechannels.out = sourcechannels.out;
      default_controls->sinkchannels.in    = sinkchannels.in;
      default_controls->sinkchannels.out   = sinkchannels.out;

			default_controls->num_controls    = num_controls;
			default_controls->num_inchannels  = num_inchannels;
			default_controls->num_outchannels = num_outchannels;

			for(i = 0, index=0, iindex=0, oindex=0; i < psDescriptor->PortCount; i++) {
				if(psDescriptor->PortDescriptors[i]&LADSPA_PORT_CONTROL) {
						default_controls->data[0+index].index = i;

						LADSPADefault(&psDescriptor->PortRangeHints[i], 44100,
								&default_controls->data[0+index].data);

					if(psDescriptor->PortDescriptors[i]&LADSPA_PORT_INPUT) {
						default_controls->data[0+index].type = LADSPA_CNTRL_INPUT;
					} else {
						default_controls->data[0+index].type = LADSPA_CNTRL_OUTPUT;
					}
					index++;

				} else if(psDescriptor->PortDescriptors[i] ==
						(LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO)) {
					default_controls->data[num_controls+iindex].index = i;
          iindex++;

				} else if(psDescriptor->PortDescriptors[i] ==
						(LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO)) {
					default_controls->data[num_controls+num_inchannels + oindex].index = i;
          oindex++;
				}
			}
#if 0
			if((default_controls->output == NULL) ||
				(default_controls->input == NULL)) {
					fprintf(stderr,
						"LADSPA Plugin must have one audio channel\n");
				free(default_controls);
				free(filename);
				return NULL;
			}
#endif

			/* Write the default data to the file. */
			if(write(fd, default_controls, length) < 0) {
				free(default_controls);
				free(filename);
				return NULL;
			}
			free(default_controls);
		} else {
			free(filename);
			return NULL;
		}
	}

	/* MMap Configuration File */
	ptr = (LADSPA_Control*)mmap(NULL, length,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close (fd);

	if(ptr == MAP_FAILED) {
		free(filename);
		return NULL;
	}

	/* Make sure we're mapped to the right file type. */
	if(ptr->length != length) {
		fprintf(stderr, "%s is the wrong length.\n",
				filename);
		LADSPAcontrolUnMMAP(ptr);
		free(filename);
		return NULL;
	}

	if(ptr->id != psDescriptor->UniqueID) {
		fprintf(stderr, "%s is not a control file for ladspa id %ld.\n",
				filename, ptr->id);
		LADSPAcontrolUnMMAP(ptr);
		free(filename);
		return NULL;
	}

	if(ptr->sourcechannels.in != sourcechannels.in || ptr->sourcechannels.out != sourcechannels.out) {
		fprintf(stderr, "%s is not a control file doesn't have %d/%d source channels.\n",
            filename, sourcechannels.in, sourcechannels.out);
		LADSPAcontrolUnMMAP(ptr);
		free(filename);
		return NULL;
	}

	if(ptr->sinkchannels.in != sinkchannels.in || ptr->sinkchannels.out != sinkchannels.out) {
		fprintf(stderr, "%s is not a control file doesn't have %d/%d sink channels.\n",
            filename, sinkchannels.in, sinkchannels.out);
		LADSPAcontrolUnMMAP(ptr);
		free(filename);
		return NULL;
	}


	free(filename);
	return ptr;
}
