
# Quiet (set to @ for a quite compile)
Q	?= @
#Q	?=

# Build Tools
CC 	:= gcc
CFLAGS += -I. -Wall -funroll-loops -ffast-math -fPIC -DPIC
LD := gcc
LDFLAGS += -Wall -shared -lasound

SND_PCM_OBJECTS = pcm_equal.o ladspa_utils.o
SND_PCM_LIBS =
SND_PCM_BIN = libasound_module_pcm_equal.so

SND_CTL_OBJECTS = ctl_equal.o ladspa_utils.o
SND_CTL_LIBS =
SND_CTL_BIN = libasound_module_ctl_equal.so

LIBDIR = lib

.PHONY: all clean dep load_default

all: Makefile $(SND_PCM_BIN) $(SND_CTL_BIN)

dep:
	@echo DEP $@
	$(Q)for i in *.c; do $(CC) -MM $(CFLAGS) "$${i}" ; done > makefile.dep

-include makefile.dep

$(SND_PCM_BIN): $(SND_PCM_OBJECTS)
	@echo LD $@
	$(Q)$(LD) $(LDFLAGS) $(SND_PCM_LIBS) $(SND_PCM_OBJECTS) -o $(SND_PCM_BIN)

$(SND_CTL_BIN): $(SND_CTL_OBJECTS)
	@echo LD $@
	$(Q)$(LD) $(LDFLAGS) $(SND_CTL_LIBS) $(SND_CTL_OBJECTS) -o $(SND_CTL_BIN)

%.o: %.c
	@echo GCC $<
	$(Q)$(CC) -c $(CFLAGS) $(CPPFLAGS) $<

clean:
	@echo Cleaning...
	$(Q)rm -vf *.o *.so

install: all
	@echo Installing...
	$(Q)mkdir -p ${DESTDIR}/usr/$(LIBDIR)/alsa-lib/
	$(Q)install -m 644 $(SND_PCM_BIN) ${DESTDIR}/usr/$(LIBDIR)/alsa-lib/
	$(Q)install -m 644 $(SND_CTL_BIN) ${DESTDIR}/usr/$(LIBDIR)/alsa-lib/

uninstall:
	@echo Un-installing...
	$(Q)rm ${DESTDIR}/usr/lib/alsa-lib/$(SND_PCM_BIN)
	$(Q)rm ${DESTDIR}/usr/lib/alsa-lib/$(SND_CTL_BIN)
	
