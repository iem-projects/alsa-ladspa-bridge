IEMLADSPA is a plugin for ALSA that loads a LADSPA plugin,
and allows control with any ALSA compatible mixer, e.g. alsamixergui.

IEMLADSPA uses any multichannel LADSPA Plugin for audio processing.
It differs from similar plugins (e.g. 'alsaequal') as it will process both the
recording and the playback streams in a single callback.
Therefore, the number of channels must match the number of input channels +
the number of output channels.
E.g. a standard soundcard will have two (2) inchannels ("microphone") and
two (2) outchannels ("speaker"), which add up to four (4) channels.
The LADSPA plugin thus needs four audio in and four audio out ports.

This should allow to build echo-cancellers and similar devices via LADSPA.

IEMLADSPA is based on 'alsaequal' by Charles Eidsness:

  http://www.thedigitalmachine.net/alsaequal.html

INSTALL
---
Download the latest version of the plugin and:

    tar xvjf iemladspa-x.x.tar.bz2
    cd iemladspa-x.x
    make
    sudo make install

DEPENDENCIES
---
- LADSPA-SDK
- ALSA Development headers and alsa-lib -- you may already have it
as part of your linux distro, you can install in Debian (and derivatives, like
Ubuntu) with 

    `sudo aptitude install libasound2-dev ladspa-sdk`


USAGE
---
After installing you will have to modify your local .asoundrc alsa
configuration file, adding something like this. If you're not using
sound card 0 modify "plughw:0,0" accordingly.

    ctl.ladspa {
    	type iemladspa;
    }
    pcm.ladspa {
        type iemladspa;
        slave.pcm "hw:0,0";
    }


You can adjust the control parameters of the plugin by using any alsa
mixer, e.g.:  

    alsamixer -D ladspa


HELP
---
If you need any help just let me know, you can reach me at:
   https://github.com/iem-projects/alsa-ladspa-bridge/issues
Please keep in mind that this is a development release and may have bugs.


More Advanced Stuff:
---
Check out the annotated [asoundrc](asoundrc) example file.

controls
--
The mixer-device ("ctl") and the audio device ("pcm") communicate via a
controls-file (as given with the 'controls' setting in asoundrc).
This file is a binary file that get's mmapped by both devices.
As such the file is fragile, and cannot be shared between two different plugins,
unless the two plugins are port-compatible.
It can however be shared between devices that use the very same LADSPA-plugins
(all instances will then be controlled simultaneously).

sample format
--
iemladspa currently only supports FLOAT and S16 format for communicating with
the soundcard ("downstream") and the application ("upstream").
So you will probably need to pump the data through a plug to change the format.

    pcm.<anothername>{
        type plug;
        slave.pcm <name>;
    }

The LADSPA-plugin will *always* work on floating point samples.
