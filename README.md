glx_hook
========

Simple tool for Linux/glibc hooking into `glXSwapInterval[EXT|SGI|MESA]` to
override VSync settings if your GPU driver doesn't allow you to override
this. With the proprietary NVIDIA Linux driver, the nvidia-settings and
`__GL_SYNC_TO_VBLANK` environment variable are actually overriden by an
application using `GLX_EXT_swap_control` or `GLX_SGI_swap_control` extensions.

This tool works by intercepting calls to those functions and exchanging the
value (or silently ignoring the calls altoghether, so that you driver
settings become effective). To do so, it is using the (in)famous `LD_PRELOAD`
approach.

There are also some more advanced features, notably a latency limiter and
a frametime measurement mode, see the section _Experimental Features_ below.

### USAGE:

    $ LD_PRELOAD=path/to/glx_hook.so GH_SWAP_MODE=$mode target_binary

or

    $ LD_PRELOAD=path/to/glx_hook_bare.so GH_SWAP_MODE=$mode target_binary

where `$mode` controls how the values are exchanged. Valid modes are
* `nop`: keep calls as intended by the application
* `ignore`: silently ignore the calls (return success to the application)
* `clamp=$x,$y`: clamp requested swap interval to [`$x`, `$y`]
* `force=$x`: always set swap interval `$x`
* `disable`: same as `force=0`
* `enable`: same as `min=1`
* `min=$x`: set interval to at least `$x`
* `max=$x`: set interval to at most `$x`

NOTE: This option only changes values forwarded to the swap interval
functions, or ignores these calls completely, but never adds new calls
to set the swap interval. If the app doesn't do it, this option does nothing.
For actually injection such calls, have a look at the experimental option
`GH_INJECT_SWAPINTERVAL` below.

NVidia is promoting a feature called "adaptive vsync" where a "late" buffer
swap is done immediately instead of beeing delayed to the next sync interval.
This feature is exposed via the `GLX_EXT_swap_control_tear` extension. If this
is present, negative intervals enable adaptive vsync with the absolute
value beeing the swap interval. The `GH_SWAP_TEAR` environment variable can
be used to control this feature: 

* `raw`: do not treat positive and negative intervals as different. This has
the effect that you for example could do a `clamp=-1,1`
* `keep`: keep the adaptive vsync setting, modify only the absoulte value
* `disable`: always disable adaptive vsync
* `enable`: always enable adaptive vsync
* `invert`: enable adaptive vsync if the app disables it and vice versa

NOTE: we do not check for the presence of this extension. Negative swap
intervals are invalid if the extension is not present. So if you enable
adaptive vsync without your driver supporting it, the calls will fail. Most
likely, the application won't care and no swap interval will be set.

Further environment variables controlling the behavior:
* `GH_VERBOSE=$level`: control level of verbosity (0 to 5)
* `GH_VERBOSE_FILE=$file`: redirect verbose output to `$file` (default is to use
			   standard error stream)

The `glx_hook.so` version is the full version which tracks GL contexts, and
allows also for glXSwapBuffers manipulations (see below). However, the GL
context tracking adds a whole layer of complexity and might fail in some
scenarios. If you are only interested in the swap interval manipulations,
you can try to use  the `glx_hook_bare.so` library, which only tries to deal
with the bare minimum of `glX` (and `dlsym`) functions.

### EXPERIMENTAL FEATURES:

The following features are only available in `glx_hook.so` (and not `glx_hook_bare.so`):

Set `GH_INJECT_SWAPINTERVAL=$n` to inject a `SwapInterval` call when a context
is made current for the first time. By default, this is disabled. The `GH_SWAP_MODE`
setting does not affect the operation of this option. This option is most
useful if the application never sets a swap interval, but it might be combined
with the other `GH_SWAP_MODE` settings, i.e. `GH_SWAP_MODE=ignore` to prevent
the app from changing th injected setting later on.

Set `GH_SWAPBUFFERS=$n` to only execute every `$n`-th buffer swap. This might be
useful for games where all game logic is implemented in the same loop as
the rendering, and you want vsync on but stilll a higher frequency for the loop.
Currently, there is no adaptive mode, so you need to have $n times the framerate
to not miss any display frames.

Set `GH_FRAMETIME=$mode` to aquire frame timings.The following modes are
supported:
* 0: no frametime measurements (the default)
* 1: measure frametime on CPU only
* 2: measure frametimes on CPU and GPU (requires a context supporting the `GL_ARB_timer_query` extension)

Use `GH_FRAMETIME_DELAY=$n` to set the delay for the timer queries (default: 3 frames).
This controls the number of frames the GPU might stay ahead of the CPU. Setting a
too low number may result in performance degradation in comparison to not measure
the frametimes.

Use `GH_FRAMETIME_FRAMES=$n` to control the number of frames which are buffered
internally (default: 1000 frames). The results will be dumped to disk if the buffer is full. Setting
a too low value here might result in performance degradation due to the output.

Use `GH_FRAMETIME_FILE=$name` to control the output file name (default:
`glx_hook_frametimes`). The string `-ctx$num.csv` will automatically be appended,
where `$num` is the number of the GL context. The output will be one line per frame,
with the following values:

    frame_number CPU GPU latency CPU GPU latency

where `CPU` denotes timestamps on the CPU, `GPU` denotes timestamps on the GPU
and `latency` denotes the latency of the GPU. All values are in nanoseconds.
The  first three values refer to the time directly before the buffer swap,
the latter to directly after the swap. The `CPU` and `GPU` values are always
relative to the buffer swap of the _previous_ frame, and `latency` is just
the observed latency at the respective timing probe. The data for frame 0 might 
be useless.

Use `GH_LATENCY=$n` to limit the number of frames the GPU lags behind. The following
values might be used:
* -2: no limit (the default)
* -1: limit to 0, force a sync right _after_ the buffer swap
* 0: limit to 0, force a sync right _before_ the buffer swap
* >0: limit the number of pending frames to `$n` (requires a context supporting the `GL_ARB_sync` extension)

This can be helpful in situations where you experience stuttering in a GL application. Preferably,
you should use `GH_LATENCY=1` to not degrade performance too much.

### INSTALLATION:

This requires glibc, as we call some internal glibc functions not intended to
be called. Tested with glibc-2.13 (from debian wheezy) and glibc-2.24
(from debian stretch). To build, just type

    $ make

(assuming you have a C compiler and the standard libs installed).
Finally copy the glx_hook.so to where you like it. For a debug build, do

    $ make DEBUG=1

Have fun,
     derhass <derhass@arcor.de>

