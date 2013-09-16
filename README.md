glx_hook
========

Simple tool for Linux/glibc hooking into glXSwapInterval[EXT|SGI|MESA] to
override VSync settings if your GPU driver doesn't allow you to override
this. With the proprietary NVIDIA Linux driver, the nvidia-settings and
__GL_SYNC_TO_VBLANK environment variable are actually overriden by an
application using GLX_EXT_swap_control or GLX_SGI_swap_control extensions.

This tool works by intercepting calls to those functions and exchanging the
value (or silently ignoring this call altoghether, so that you driver
settings become effective). To do so, it is using the (in)famous LD_PRELOAD
approch.

# USAGE:

$ LD_PRELOAD=path/to/glx_hook.so GH_SWAP_MODE=_mode_ target_binary

where _mode_ controls how the values are exchanged. Valid modes are
* nop: keep calls as intended by the application
* ignore: silently ignore the calls (return success to the application)
* clamp=_x_,_y_: clamp requested swap interval to [_x_,_y_]
* force=_x_: always set swap interval _x_
* disable: same as force=0
* enable: same as min=1
* min=_x_: set interval to at least _x_
* max=_x_: set interval to at most _x_

NOTE: The tool currently only changes values forwarded to the swap interval
functions, or ignores these calls completely, but never adds new calls
to set the swap interval. If the app doesn't do it, this tool does nothing.

Further enivornmet variables controlling the behavior
* GH_VERBOSE=_level_: control level of verbosity (0 to 5)
* GH_VERBOSE_FILE=<file>: redirect verbose output to <file> (default is to use
			   standard error stream)

# INSTALLATION:

This requires glibc, as we call some internal glibc functions not intended to
be called. Tested witrh glibc-2.13 (from debian wheezy). To build, just type
(assuming you have a C compiler and the standard libs installed)

$ make

Finally copy the glx_hook.so to where you like it. For a debug build, do
$ make DEBUG=1

Have fun,
     derhass <derhass@arcor.de>

