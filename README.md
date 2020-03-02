glx_hook
========

Simple tool for Linux/glibc hooking into OpenGL functions.

The main motivation for this was intercepting application calls to
`glXSwapInterval[EXT|SGI|MESA]` to override VSync settings if your GPU driver
doesn't allow you to override this.
With the proprietary NVIDIA Linux driver, the nvidia-settings and
`__GL_SYNC_TO_VBLANK` environment variable are actually overriden by an
application using
[`GLX_EXT_swap_control`](https://www.opengl.org/registry/specs/EXT/swap_control.txt),
[`GLX_SGI_swap_control`](https://www.opengl.org/registry/specs/SGI/swap_control.txt)
or `GLX_MESA_swap_control` extensions.

This tool works by exchanging the
value (or silently ignoring the calls altoghether, so that you driver
settings become effective). To do so, it is using the (in)famous `LD_PRELOAD`
approach.

There are also some more advanced features, notably a latency limiter and
a frametime measurement mode, see the section [Experimental Features](#experimental-features) below.

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

**NOTE**: This option only changes values forwarded to the swap interval
functions, or ignores these calls completely, but never adds new calls
to set the swap interval. If the app doesn't do it, this option does nothing.
For actually injection such calls, have a look at the experimental option
[`GH_INJECT_SWAPINTERVAL`](#swap-interval-injection) below.

NVidia is promoting a feature called "adaptive vsync" where a "late" buffer
swap is done immediately instead of being delayed to the next sync interval.
This feature is exposed via the
[`GLX_EXT_swap_control_tear`](https://www.opengl.org/registry/specs/EXT/glx_swap_control_tear.txt)
extension. If this
is present, negative intervals enable adaptive vsync with the absolute
value beeing the swap interval. The `GH_SWAP_TEAR` environment variable can
be used to control this feature: 

* `raw`: do not treat positive and negative intervals as different. This has
the effect that you for example could do a `clamp=-1,1`
* `keep`: keep the adaptive vsync setting, modify only the absoulte value
* `disable`: always disable adaptive vsync
* `enable`: always enable adaptive vsync
* `invert`: enable adaptive vsync if the app disables it and vice versa (whatever this might be useful for...)

**NOTE**: we do not check for the presence of this extension. Negative swap
intervals are invalid if the extension is not present. So if you enable
adaptive vsync without your driver supporting it, the calls will fail. Most
likely, the application won't care and no swap interval will be set.

Further environment variables controlling the behavior:
* `GH_VERBOSE=$level`: control level of verbosity (0 to 5)
* `GH_VERBOSE_FILE=$file`: redirect verbose output to `$file` (default is to use
			   standard error stream), see section [File Names](#file-names)
			   for details about how the file name is parsed

The `glx_hook.so` version is the full version which tracks GL contexts, and
allows also for `glXSwapBuffers` manipulations (see below). However, the GL
context tracking adds a whole layer of complexity and might fail in some
scenarios. If you are only interested in the swap interval manipulations,
you can try to use  the `glx_hook_bare.so` library, which only tries to deal
with the bare minimum of `glX` (and `dlsym`) functions.

#### Extra Options

If a GL symbol cannot be resolved, glx_hook tries to manually load the
OpenGL library via `dlopen(3)`. This behavior can be controlled by the
`GH_LIBGL_FILE` environment variable: If it is not set, `libGL.so` is
used, but you might want to specify another one, potentially with
full path. If you set `GH_LIBGL_FILE=""`, libGL loading is disabled.

### EXPERIMENTAL FEATURES

The following features are only available in `glx_hook.so` (and not `glx_hook_bare.so`):

#### Swap Interval Injection

Set `GH_INJECT_SWAPINTERVAL=$n` to inject a `SwapInterval` call when a context
is made current for the first time. By default, this is disabled. The `GH_SWAP_MODE`
setting does not affect the operation of this option. This option is most
useful if the application never sets a swap interval, but it might be combined
with the other `GH_SWAP_MODE` settings, i.e. `GH_SWAP_MODE=ignore` to prevent
the app from changing th injected setting later on.

#### Frame timing measurement / benchmarking

Set `GH_FRAMETIME=$mode` to aquire frame timings.The following modes are
supported:
* `0`: no frametime measurements (the default)
* `1`: measure frametime on CPU only
* `2`: measure frametimes on CPU and GPU (requires a context >= 3.3, or supporting the
[`GL_ARB_timer_query`](https://www.opengl.org/registry/specs/ARB/timer_query.txt)
extension)

Use `GH_FRAMETIME_DELAY=$n` to set the delay for the timer queries (default: 10 frames).
This controls the number of frames the GPU might lag behind of the CPU. Setting a
too low number may result in performance degradation in comparison to not measuring
the frametimes. The implicit synchronizations can have a similar effect as the
[`GH_LATENCY`](#latency-limiter) setting, albeit only as an unintented side-effect,
and might completely invalidate the measurements. Just leave this value
at the default unless you know exactly what you are doing...

Use `GH_FRAMETIME_FRAMES=$n` to control the number of frames which are buffered
internally (default: 1000 frames). The results will be dumped to disk if the buffer is full. Setting
a too low value here might result in performance degradation due to the output.

Use `GH_FRAMETIME_FILE=$name` to control the output file name (default:
`glx_hook_frametimes-ctx%c.csv`). See section [File Names](#file-names)
for details about how the file name is parsed.
The output will be one line per frame,
with the following values:

    frame_number CPU GPU latency CPU GPU latency

where `CPU` denotes timestamps on the CPU, `GPU` denotes timestamps on the GPU
and `latency` denotes the latency of the GPU. All values are in nanoseconds.
The  first three values refer to the time directly before the buffer swap,
the latter to directly after the swap. The `CPU` and `GPU` values are always
relative to the buffer swap of the _previous_ frame, and `latency` is just
the observed latency at the respective timing probe. The data for frame 0 might 
be useless.

Included is an example script for [gnuplot](http://www.gnuplot.info),
[`script.gnuplot`](https://raw.githubusercontent.com/derhass/glx_hook/master/script.gnuplot),
to easily create some simple frame timing graphs. You can use it directly
on any frametime file by specifying the `filename` variable on the gnuplot command line:

    gnuplot -e filename=\'glx_hook_frametimes-ctx1.csv\' script.gnuplot

#### Latency Limiter

Use `GH_LATENCY=$n` to limit the number of frames the GPU lags behind. The following
values might be used:
* `-2`: no limit (the default)
* `-1`: limit to 0, force a sync right _after_ the buffer swap
* `0`: limit to 0, force a sync right _before_ the buffer swap
* `>0`: limit the number of pending frames to `$n` (requires a context >= 3.2,
or supporting the
[`GL_ARB_sync`](https://www.opengl.org/registry/specs/ARB/sync.txt)
extension)

This can be helpful in situations where you experience stuttering in a GL application. Preferably,
you should use `GH_LATENCY=1` to not degrade performance too much.

Some GL drivers may use busy waiting when waiting for the sync objects, resulting
in maxing out one CPU core for nothing. If this is an issue, you can try setting
`GH_LATENCY_WAIT_USECS` to some value > 0. This will disbale waiting for the
completion of the sync object directly and instead use a loop to query the
completion status, and add a sleep cycle for that many microseconds wheever the
sync object was found not completed. Useful values should be in the range
of 100 to 2000 usecs, but even setting it to 1 may have some effect.

#### Buffer Swap Omission

Set `GH_SWAPBUFFERS=$n` to only execute every `$n`-th buffer swap. This might be
useful for games where all game logic is implemented in the same loop as
the rendering, and you want vsync on but stilll a higher frequency for the loop.
Currently, there is no adaptive mode, so you need to have`$n` times the framerate
to not miss any display frames.

#### Sleep injection

Set `GH_SWAP_SLEEP_USECS=$n` to force an addition sleep of that many microseconds
after each buffer swap. This might be useful if you want to reduce the framerate or simulate
a slower machine.

#### GL Context attribute overrides

You can override the attributes for GL context creation. This will require the
[`GLX_ARB_create_context`](https://www.khronos.org/registry/OpenGL/extensions/ARB/GLX_ARB_create_context.txt)
extension. The following overrides are defined:
* `GH_FORCE_GL_VERSION_MAJOR`: set the the GL major version number to request
* `GH_FORCE_GL_VERSION_MINOR`: set the the GL minor version number to request
* `GH_FORCE_GL_CONTEXT_PROFILE_CORE`: set to non-zero to force the creation of a core profile. (Requires GL version of at least 3.2)
* `GH_FORCE_GL_CONTEXT_PROFILE_COMPAT`: set to non-zero to force the creation of a compat profile profile. (Requires GL version of at least 3.2)
* `GH_FORCE_GL_CONTEXT_FLAGS_NO_DEBUG`: set to non-zero to disable debug contexts.
* `GH_FORCE_GL_CONTEXT_FLAGS_DEBUG`: set to non-zero to force debug contexts. `GH_FORCE_GL_CONTEXT_FLAGS_DEBUG` takes precedence over `GH_FORCE_GL_CONTEXT_FLAGS_NO_DEBUG`.
* `GH_FORCE_GL_CONTEXT_FLAGS_NO_FORWARD_COMPAT`: set to non-zero to disable forwadr-compatible contexts.
* `GH_FORCE_GL_CONTEXT_FLAGS_FORWARD_COMPAT`: set to non-zero to force forward-compatible contexts. `GH_FORCE_GL_CONTEXT_FLAGS_FORWARD_COMPAT` takes precedence over `GH_FORCE_GL_CONTEXT_FLAGS_NO_FORWARD_COMPAT`.

You can also directly specify the bitmasks for the context flags and profile mask (see the various `GLX` context creation extensions for the actual values):
* `GH_FORCE_GL_CONTEXT_FLAGS_ON` manaully specify a the bits which must be forced on in the context flags bitmask.
* `GH_FORCE_GL_CONTEXT_FLAGS_OFF` manaully specify a the bits which must be forced off in the context flags bitmask.
* `GH_FORCE_GL_CONTEXT_PROFILE_MASK_ON` manaully specify a the bits which must be forced on in the context profile bitmask.
* `GH_FORCE_GL_CONTEXT_PROFILE_MASK_OFF` manaully specify a the bits which must be forced off in the context profile bitmask.
When setting these, they will override any settings by the other `GL_FORCE_GL_*` environment variables.

Note that the context profile is only relevant for GL version 3.2 and up. When forcing a GL version
of 3.2 or higher, the default profile is the core profile. YOu must explicitely request a compat profile
if the application would otherwise work with a leagcy context (by not using `GLX_ARB_create_context` or
specifying an earlier version).

#### GL Debug Output

By setting `GH_GL_DEBUG_OUTPUT` to a non-zero value, [GL debug output](https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_debug_output.txt) message callbacks will be intercepted. The debug messages will be logged as `INFO` level messages in the GH log. Set `GH_GL_INJECT_DEBUG_OUTPUT` to a non-zero value to inject a call to the
debug output functionality into the application. Note that to get debug output, you must
force the creation of a debug GL context if the app does not do it on its own.

### FILE NAMES

Whenever an output file name is specified, special run-time information
can be inserted in the file name to avoid overwriting previous files in
complex situations (i.e. the application is using several processes).
A sequence of `%` followed by another character is treated depending
on the second character as follows:

* `c`: the GL context number (sequetially counted from 0), (this is not
		available for the `GH_VERBOSE_FILE` output, context number is
		always 0 there)
* `p`: the PID of the process
* `t`: the current timestamp as `<seconds_since_epoch>.<nanoseconds>`
* `%`: the `%` sign itself

### INSTALLATION:

This requires glibc, as we call some internal glibc functions not intended to
be called. Tested with glibc-2.13 (from debian wheezy), glibc-2.24
(from debian stretch) and glibc-2.28 (from debian buster). To build, just type

    $ make

(assuming you have a C compiler and the standard libs installed).
Finally copy the `glx_hook.so` to where you like it. For a debug build, do

    $ make DEBUG=1

Have fun,
     derhass <derhass@arcor.de>

