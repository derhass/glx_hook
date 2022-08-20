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
in maxing out one CPU core for only very little of an advantage. However, sometimes you might
even want to use busy waiting (even if the driver doesn't do it for you). Two different
modes are implemented:
* the _standard_ mode, which uses a single call to `glClientWaitSync`,
  which wait for either the completion of rendering of the relevant frame, or the reaching of
  a timeout, whatever comes first. The timeout can be specified by setting
  `GH_LATENCY_GL_WAIT_TIMEOUT_USECS=$n`, where `$n` is the timeout in microseconds. Default is
  `1000000` (1 second), but you can turn this down significantly if you don't wait for
  long periods even in extreme cases. Setting this too low might result in not actually
  full synchronization.
* the _manual_ mode, where the wait is performed in a loop, until synchronization is
  achieved. Use `GH_LATENCY_GL_WAIT_USECS=$n` to set the wait timeout for each individual
  GL wait operation to `$n` microseconds (default: 0) and `GH_LATENCY_WAIT_USECS=$n`
  to add an additional _sleep_ cycle of `$n` microseconds per loop iteration (default: 0).

The mode is selected by setting `GH_LATENCY_MANUAL_WAIT=$n`, where `$n` is
* `-1`: automatic mode selecion  (the default):  enable manual mode if either of `GH_LATENCY_GL_WAIT_USECS` or `GH_LATENCY_WAIT_USECS`
is set to a non-zero value
* `0`: always use standard mode
* `1`: always use manual mode (this allows explicit busy waiting by setting both wait usecs to 0)

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
* `GH_FORCE_MIN_GL_VERSION_MAJOR`: set the the minimum GL major version number to request
* `GH_FORCE_MIN_GL_VERSION_MINOR`: set the the minimum GL minor version number to request
* `GH_FORCE_MAX_GL_VERSION_MAJOR`: set the the maximum GL major version number to request
* `GH_FORCE_MAX_GL_VERSION_MINOR`: set the the maximum GL minor version number to request
* `GH_FORCE_GL_VERSION_MAJOR`: set the the exact GL major version number to request
* `GH_FORCE_GL_VERSION_MINOR`: set the the exact GL minor version number to request
* `GH_FORCE_GL_CONTEXT_PROFILE_CORE`: set to non-zero to force the creation of a core profile. (Requires GL version of at least 3.2)
* `GH_FORCE_GL_CONTEXT_PROFILE_COMPAT`: set to non-zero to force the creation of a compat profile. (Requires GL version of at least 3.2). Set to 1 to always force compat, and to 2 only if the app would be using legacy instead.
* `GH_FORCE_GL_CONTEXT_FLAGS_NO_DEBUG`: set to non-zero to disable debug contexts.
* `GH_FORCE_GL_CONTEXT_FLAGS_DEBUG`: set to non-zero to force debug contexts. `GH_FORCE_GL_CONTEXT_FLAGS_DEBUG` takes precedence over `GH_FORCE_GL_CONTEXT_FLAGS_NO_DEBUG`.
* `GH_FORCE_GL_CONTEXT_FLAGS_NO_FORWARD_COMPAT`: set to non-zero to disable forwadr-compatible contexts.
* `GH_FORCE_GL_CONTEXT_FLAGS_FORWARD_COMPAT`: set to non-zero to force forward-compatible contexts. `GH_FORCE_GL_CONTEXT_FLAGS_FORWARD_COMPAT` takes precedence over `GH_FORCE_GL_CONTEXT_FLAGS_NO_FORWARD_COMPAT`.
* `GH_FORCE_GL_CONTEXT_FLAGS_NO_ERROR`: set to non-zero to force a no-error context (as defined in [`GL_KHR_NO_ERRORi`](https://registry.khronos.org/OpenGL/extensions/KHR/KHR_no_error.txt)).
* `GH_FORCE_GL_CONTEXT_FLAGS_ERROR`: set to non-zero to force removal of the no-error context flag if the application may request that.

The GL version overrides are applied in the order `min,max,exact`. Set a component to `-1` for no override.
Note: it is advised to set both the major and minor version, the version comparision will then take
both major and minor into account (e.g. a minumium of 3.2 will not change a requested 4.0 to 4.2),
but it is also possible to override only one component if you really want to (e.g. a minimum of -1.2 will
change a requested 4.0 to 4.2).

You can also directly specify the bitmasks for the context flags and profile mask (see the various `GLX` context creation extensions for the actual values):
* `GH_FORCE_GL_CONTEXT_FLAGS_ON` manaully specify a the bits which must be forced on in the context flags bitmask.
* `GH_FORCE_GL_CONTEXT_FLAGS_OFF` manaully specify a the bits which must be forced off in the context flags bitmask.
* `GH_FORCE_GL_CONTEXT_PROFILE_MASK_ON` manaully specify a the bits which must be forced on in the context profile bitmask.
* `GH_FORCE_GL_CONTEXT_PROFILE_MASK_OFF` manaully specify a the bits which must be forced off in the context profile bitmask.
When setting these, they will override any settings by the other `GL_FORCE_GL_*` environment variables.

Note that the context profile is only relevant for GL version 3.2 and up. When forcing a GL version
of 3.2 or higher, the default profile is the core profile. You must explicitely request a compat profile
if the application would otherwise work with a leagcy context (by not using `GLX_ARB_create_context` or
specifying an earlier version), or use `GH_FORCE_GL_CONTEXT_PROFILE_COMPAT=2` to dynamically request
compatibility profile only if legacy profiles were requested.

#### GL Debug Output

By setting `GH_GL_DEBUG_OUTPUT` to a non-zero value, [GL debug output](https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_debug_output.txt) message callbacks will be intercepted. The debug messages will be logged as `INFO` level messages in the GH log. Set `GH_GL_INJECT_DEBUG_OUTPUT` to a non-zero value to inject a call to the
debug output functionality into the application. Note that to get debug output, you must
force the creation of a debug GL context if the app does not do it on its own.

See the example script [`setup_env_debug`](https://github.com/derhass/glx_hook/blob/master/setup_env_debug)
for a setup which tries to inject Debug Output into an unspecified GL app.

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

To build, just type

    $ make

(assuming you have a C compiler and the standard libs installed).
Finally copy the `glx_hook.so` to where you like it. For a debug build, do

    $ make DEBUG=1

glx_hook requires glibc, as we rely on some glibc internas.
Tested with glibc-2.13 (from debian wheezy), glibc-2.24
(from debian stretch) and glibc-2.28 (from debian buster).

#### Hooking mechanism

glx_hook works by exporting all the relevant GL functions in the shared object,
as well as hooking into `dlsym()` (and optionally also `dlvsym()`) as well as
`glXGetProcAddress`/`glXGetProcAddressARB`.

Howver, hooking `dlsym()`/`dlvsym()` can be done via different methods,
The method is selected at compile time via the `METHOD` variable:

    $ make METHOD=2

The following methods are available:

* `1`: Deprecated: Use the internal `_dl_sym()` function of glibc. However, this function
  is not exported any more since glibc-2.34, so this approach won't work with
  newer linux distros beginning some time around autumn of 2021.
  Using this method allows for hooking `dlsym()` and `dlvsym`.

* `2`: Use the `dlvsym()` function which is an official part of the glibc API and ABI.
  To query the original `dlsym` via `dlvsym`, we need to know the exact version
  of the symbol, which in glibc is dependent on the platform.
  glx_hook currently supports the platforms x86_64 and i386 via this method,
  but other platforms can easyly be added. Just do a
  `grep 'GLIBC_.*\bdlsym\b' -r sysdeps` in the root folder of the glibc source.
  Using this methid allows for hooking `dlsym()`, but not `dlvsym`.
  This is currently the default.
 
* `3`: Use a second helper library `dlsym_wrapper.so`. That file will be automatically
  built if this mode is selected. It must be placed in the same folder where
  the `glx_hook.so` is located, and will be dynamically loaded at runtime when
  `glx_hook.so` initializes itself. Using this method allows for hooking `dlsym()` and `dlvsym`.
  It is probably the most flexible approach, but it adds some complexity.

When using the method 2, this means that we end up getting the symbol
from `glibc` even if another hooking library is injected to the same process.
By default, glx_hooks plays nice and actually uses the `dlsym()` queried by
`dlvsym()` to again query for the unversioned `dlsym`. This behavior can
be prevented by setting the `GH_ALLOW_DLSYM_REDIRECTION` environment variable
to 0. It is only relevant for `METHOD=2`.

You can control wether we shall also hook the `dlsym()` and `dlvsym()` methods
dynamically, meaning an application calling (our) `dlsym()` to query for `"dlsym"` itself
should be redirected to our implementation. Use `GH_HOOK_DLSYM_DYNAMICALLY=1` or
`GH_HOOK_DLVSYM_DYNAMICALLY=1` to enable is. Bu default, this is disabled, as this
creates lots of shenanigans, especially if we are not the only `dlsym`/`dlvsym` hook
around. Use with care.

### EXAMPLES

There are some example scripts to simplify the setup:
* [`setup_env`](https://github.com/derhass/glx_hook/blob/master/setup_env)
* [`setup_env_debug`](https://github.com/derhass/glx_hook/blob/master/setup_env_debug)

Use either of these to set up your current shell's environment for the use of `glx_hook.so`:
* `cd /path/to/your/glx_hook/installation; source ./setup_env` (assumes the `setup.env` and the `glx_hook.so` are in the same directory)
* `source /path/to/your/glx_hook/scripts/setup_env /path/to/your/glx_hook/installation` (directories might differ)

Have fun,
     derhass
     (<derhass@arcor.de>)

