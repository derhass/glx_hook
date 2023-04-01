#define _GNU_SOURCE
#include <dlfcn.h>	/* for RTLD_NEXT */
#include <pthread.h>	/* for mutextes */
#include <unistd.h>	/* for usleep(3) */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>

#include <GL/glx.h>
#include <GL/glxext.h>
#include <math.h>

#ifdef GH_CONTEXT_TRACKING
#include <time.h>	/* for clock_gettime */
#include <GL/glext.h>
#endif

#include "dlsym_wrapper.h"

#ifdef __GLIBC__
#if (__GLIBC__ > 2) || ( (__GLIBC__ == 2 ) && (__GLIBC_MINOR__ >= 34))
/* glibc >= 2.34 does not export _dl_sym() any more, we MUST use the dlvsym approach */
#if (GH_DLSYM_METHOD == 1) 
#error GH_DLSYM_METHOD 1 is not available with this libc version
#endif
#endif
#endif

/* we use this value as swap interval to mark the situation that we should
 * not set the swap interval at all. Note that with
 * GLX_EXT_swap_control_tear, negative intervals are allowed, and the
 * absolute value specifies the real interval, so we use just INT_MIN to
 * avoid conflicts with values an application might set. */
#define GH_SWAP_DONT_SET	INT_MIN
#define GL_MAX_ANISOTROPY_EXT 0x84FE

/***************************************************************************
 * GH_DLSYM_METHOD selection                                               *
 ***************************************************************************/

/* supported methods:
 * 0: _dl_sym()
 * 1: dlvsym()
 * 2: dlsym_wrapper.so
 * See README.md for details
 */
#if (GH_DLSYM_METHOD == 1) /* METHOD 1*/
#define GH_DLSYM_NEED_LOCK /* METHOD 2*/
#elif (GH_DLSYM_METHOD == 2)
#if defined(__x86_64__) /* platforms */
#define GH_DLSYM_ABI_VERSION "2.2.5"
#elif defined(__i386__)
#define GH_DLSYM_ABI_VERSION "2.0"
#else
/* if you need support for your platform, get the exact version for dlsym
 * for the glibc ABI of your platform */
#error platform not supported
#endif /* platforms */
#elif (GH_DLSYM_METHOD == 3) /* METHOD 3*/
#define GH_DLSYM_NEED_LOCK
#else
#error GH_DLSYM_METHOD not supported
#endif /* GH_DLSYM_METHOD */

/***************************************************************************
 * helpers                                                                 *
 ***************************************************************************/

static const char *
get_envs(const char *name, const char *def)
{
	const char *s=getenv(name);
	return (s)?s:def;
}

static int
get_envi(const char *name, int def)
{
	const char *s=getenv(name);
	int i;

	if (s) {
		i=(int)strtol(s,NULL,0);
	} else {
		i=def;
	}
	return i;
}

#ifdef GH_CONTEXT_TRACKING

static unsigned int
get_envui(const char *name, unsigned int def)
{
	const char *s=getenv(name);
	int i;

	if (s) {
		i=(unsigned)strtoul(s,NULL,0);
	} else {
		i=def;
	}
	return i;
}

#endif

static size_t
buf_printf(char *buf, size_t pos, size_t size, const char *fmt, ...)
{
	va_list args;
	size_t left=size-pos;
	int r;

	va_start(args, fmt);
	r=vsnprintf(buf+pos, left, fmt, args);
	va_end(args);

	if (r > 0) {
		size_t written=(size_t)r;
		pos += (written >= left)?left:written;
	}
	return pos;
}

static void
parse_name(char *buf, size_t size, const char *name_template, unsigned int ctx_num)
{
	struct timespec ts_now;
	int in_escape=0;
	size_t pos=0;
	char c;

	buf[--size]=0; /* resverve space for final NUL terminator */
	while ( (pos < size) && (c=*(name_template++)) ) {
		if (in_escape) {
			switch(c) {
				case '%':
					buf[pos++]=c;
					break;
				case 'c':
					pos=buf_printf(buf,pos,size,"%u",ctx_num);
					break;
				case 'p':
					pos=buf_printf(buf,pos,size,"%u",(unsigned)getpid());
					break;
				case 't':
					clock_gettime(CLOCK_REALTIME, &ts_now);
					pos=buf_printf(buf,pos,size,"%09lld.%ld",
							(long long)ts_now.tv_sec,
							ts_now.tv_nsec);
					break;
				default:
					pos=buf_printf(buf,pos,size,"%%%c",c);
			}
			in_escape=0;
		} else {
			switch(c) {
				case '%':
					in_escape=1;
					break;
				default:
					buf[pos++]=c;
			}
		}
	}
	buf[pos]=0;
}

/***************************************************************************
 * MESSAGE OUTPUT                                                          *
 ***************************************************************************/

typedef enum {
	GH_MSG_NONE=0,
	GH_MSG_ERROR,
	GH_MSG_WARNING,
	GH_MSG_INFO,
	GH_MSG_DEBUG,
	GH_MSG_DEBUG_INTERCEPTION
} GH_msglevel;

#ifdef NDEBUG
#define GH_MSG_LEVEL_DEFAULT GH_MSG_WARNING
#else
#define GH_MSG_LEVEL_DEFAULT GH_MSG_DEBUG_INTERCEPTION
#endif

#define GH_DEFAULT_OUTPUT_STREAM stderr

static void GH_verbose(int level, const char *fmt, ...)
{
	static int verbosity=-1;
	static FILE *output_stream=NULL;
	static int stream_initialized=0;
	va_list args;

	if (verbosity < 0) {
		verbosity=get_envi("GH_VERBOSE", GH_MSG_LEVEL_DEFAULT);
	}

	if (level > verbosity) {
		return;
	}

	if (!stream_initialized) {
		const char *file=getenv("GH_VERBOSE_FILE");
		if (file) {
			char buf[PATH_MAX];
			parse_name(buf, sizeof(buf), file, 0);
			output_stream=fopen(buf,"a+t");
		}
		if (!output_stream)
			output_stream=GH_DEFAULT_OUTPUT_STREAM;
		stream_initialized=1;
	}
	fprintf(output_stream,"GH: ");
	va_start(args, fmt);
	vfprintf(output_stream, fmt, args);
	va_end(args);
	fflush(output_stream);
}

/***************************************************************************
 * FUNCTION INTERCEPTOR LOGIC                                              *
 ***************************************************************************/

typedef void (*GH_fptr)();
typedef void * (*GH_resolve_func)(const char *);

#ifdef GH_DLSYM_NEED_LOCK
/* mutex used during GH_dlsym_internal () */
static pthread_mutex_t GH_mutex=PTHREAD_MUTEX_INITIALIZER;
#endif

#if (GH_DLSYM_METHOD == 1)
/* THIS IS AN EVIL HACK: we directly call _dl_sym() of the glibc
 * NOTE: the approrpiate function prototype could be
 *       extern void *_dl_sym(void *, const char *, void (*)() );
 *       but we use it only with some specific argument for the
 *       last parameter, and use the function pointer type
 *       directly in this declaration, which avoids us to do a
 *       cast between (technically incompatible) function pointer
 *       types. Hiding the cast in the declatation is of course
 *       as broken as before from a C correctness point of view,
 *       but the compiler won't notice any more... ;) */
extern void *_dl_sym(void *, const char *, void* (*)(void *, const char *) );
#elif (GH_DLSYM_METHOD == 3)
/* Use the dlsym_wrapper. We can use dlopen(), but we can not
 * use dlsym() or dlvsym(), as these functions are hooked by ourselves.
 * However, dlsym_wrapper will execute an intialization function when
 * dlopen() is called. The trick is the we load dlsym_wrapper.so
 * with RTLD_LOCAL | RTLD_DEEPBIND, so that it gets its own private
 * linker map to the libdl.so, without falling back on the dlsym()
 * defined here. It then forwards the pointer via an environment
 * variable (as hex via printf %p format string) */
static void *dlsym_wrapper_get(void* handle, const char *name)
{
	char dlsym_wrapper_name[4096];
	void *wrapper;
	const char * ptr_str;
	void *res = NULL;
	void *ptr = NULL;
	Dl_info self;
	size_t len;
	char *pos;
	size_t idx;
	size_t slen;

	(void)handle;
	(void)name;

	memset(&self,0, sizeof(self));
	if (!dladdr(dlsym_wrapper_get, &self)) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: failed to find myself!\n");
		return NULL;
	}
	if (!self.dli_fname) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: failed to find my path!\n");
		return NULL;
	}
	GH_verbose(GH_MSG_DEBUG,"dlsym_wrapper: I am at '%s'\n", self.dli_fname);
	len = strlen(self.dli_fname);
	if (len >= sizeof(dlsym_wrapper_name)) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: my path '%s' is too long: %zu >= %zu\n", self.dli_fname, len, sizeof(dlsym_wrapper_name));
		return NULL;
	}
	memcpy(dlsym_wrapper_name, self.dli_fname, len+1);
	pos = strrchr(dlsym_wrapper_name, '/');
	if (pos) {
		idx = pos - dlsym_wrapper_name + 1;
	} else {
		idx = 0;
	}
	if (idx > len) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: failed to process my path '%s'\n", dlsym_wrapper_name);
		return NULL;
	}
	len = sizeof(dlsym_wrapper_name) - idx;
	slen = strlen(DLSYM_WRAPPER_NAME) + 1;
	if (len < slen) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: failed to build path to the wrapper library: avail %zu < needed %zu\n", len, slen);
		return NULL;
	}
	memcpy(dlsym_wrapper_name + idx, DLSYM_WRAPPER_NAME, slen);
	GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: wrapper library should be at '%s'\n", dlsym_wrapper_name);

	if (getenv(DLSYM_WRAPPER_ENVNAME)) {
		GH_verbose(GH_MSG_WARNING,"dlsym_wrapper: '%s' already defined, shouldn't be\n", DLSYM_WRAPPER_ENVNAME);
	}
	wrapper = dlopen(dlsym_wrapper_name, RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND | RTLD_NOLOAD);
	if (wrapper) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: '%s' already loaded, this does not work!\n", dlsym_wrapper_name);
		dlclose(wrapper);
		return NULL;
	}
	wrapper = dlopen(dlsym_wrapper_name, RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
	if (!wrapper) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: '%s' could not be loaded!\n", dlsym_wrapper_name);
		return NULL;
	}

	ptr_str = getenv(DLSYM_WRAPPER_ENVNAME);
	if (!ptr_str) {
		GH_verbose(GH_MSG_ERROR,"dlsym_wrapper: '%s' was not defined by the wrapper library\n", DLSYM_WRAPPER_ENVNAME);
		dlclose(wrapper);
		return NULL;
	}
	GH_verbose(GH_MSG_DEBUG, "dlsym_wrapper: got '%s'='%s'\n", DLSYM_WRAPPER_ENVNAME, ptr_str);
	if (sscanf(ptr_str, "%p", &ptr) == 1) {
		if (ptr) {
			GH_verbose(GH_MSG_DEBUG, "dlsym_wrapper: using %p as original dlsym()\n", ptr);
			res = ptr;
		} else {
			GH_verbose(GH_MSG_ERROR, "dlsym_wrapper: original dlsym() pointer is invalid\n", ptr);
		}
	} else {
		GH_verbose(GH_MSG_WARNING,"dlsym_wrapper: failed to parse pointer from '%s'='%s'\n", DLSYM_WRAPPER_ENVNAME, ptr_str);
	}

	dlclose(wrapper);
	if (res) {
		GH_verbose(GH_MSG_ERROR, "dlsym_wrapper: successfilly queried '%s' = %p\n", name, res);
	} else {
		GH_verbose(GH_MSG_ERROR, "dlsym_wrapper: failed to query '%s'\n", name);
	}
	return res;
}
#endif

/* Mutex for the function pointers. We only guard the
 * if (ptr == NULL) ptr=...; part. The pointers will never
 * change after being set to a non-NULL value for the first time,
 * so it is safe to dereference them without locking */
static pthread_mutex_t GH_fptr_mutex=PTHREAD_MUTEX_INITIALIZER;

/* Wrapper function called in place of dlsym(), since we intercept dlsym().
 * We use this ONLY to get the original dlsym() itself, all other symbol
 * resolutions are done via that original function, then.
 */
static void *GH_dlsym_internal(void *handle, const char *name)
{
	void *ptr;

#ifdef GH_DLSYM_NEED_LOCK
	/* ARGH: we are bypassing glibc's locking for dlsym(), so we
	 * must do this on our own */
	pthread_mutex_lock(&GH_mutex);
#endif

#if (GH_DLSYM_METHOD == 1)
	GH_verbose(GH_MSG_DEBUG, "using _dl_sym() method\n");

	/* Third argument is the address of the caller, (glibc uses stack
	 * unwinding internally to get this),  we just use the address of our
	 * wrapper function itself, which is wrong when this is called on
	 * behalf of the real application doing a dlsycm, but we do not
	 *  care... */
	ptr=_dl_sym(handle, name, GH_dlsym_internal);
#elif (GH_DLSYM_METHOD == 2)
	GH_verbose(GH_MSG_DEBUG, "using dlvsym() method\n");
	ptr=dlvsym(handle, name, "GLIBC_" GH_DLSYM_ABI_VERSION);
#elif (GH_DLSYM_METHOD == 3)
	GH_verbose(GH_MSG_DEBUG, "using dlsym_wrapper.so method\n");
	ptr=dlsym_wrapper_get(handle, name);
#else
#error GH_DLSYM_METHOD not supported
#endif /* GH_DLSYM_METHOD */

#ifdef GH_DLSYM_NEED_LOCK
	pthread_mutex_unlock(&GH_mutex);
#endif
	return ptr;
}

/* Wrapper funtcion to query an original function avoiding
 * recursively calls to the interceptor dlsym() below */
static void *GH_dlsym_internal_next(const char *name)
{
	return GH_dlsym_internal(RTLD_NEXT, name);
}

/* return intercepted function pointer for a symbol */
static void *GH_get_interceptor(const char*, GH_resolve_func, const char *);

/* function pointers to call the real functions that we did intercept */
static void * (* volatile GH_dlsym)(void *, const char*)=NULL;
static void * (* volatile GH_dlvsym)(void *, const char*, const char *)=NULL;
static GH_fptr (* volatile GH_glXGetProcAddress)(const char*)=NULL;
static GH_fptr (* volatile GH_glXGetProcAddressARB)(const char *)=NULL;
static void (* volatile GH_glXSwapBuffers)(Display *, GLXDrawable);
static void (* volatile GH_glXSwapIntervalEXT)(Display *, GLXDrawable, int);
static int (* volatile GH_glXSwapIntervalSGI)(int);
static int (* volatile GH_glXSwapIntervalMESA)(unsigned int);
static GLXContext (* volatile GH_glXCreateContext)(Display*, XVisualInfo *, GLXContext, Bool);
static GLXContext (* volatile GH_glXCreateNewContext)(Display *, GLXFBConfig, int, GLXContext, Bool);
static GLXContext (* volatile GH_glXCreateContextAttribsARB)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
static GLXContext (* volatile GH_glXImportContextEXT)(Display *, GLXContextID);
static GLXContext (* volatile GH_glXCreateContextWithConfigSGIX)(Display *, GLXFBConfigSGIX, int, GLXContext, Bool);
static void (* volatile GH_glXDestroyContext)(Display *, GLXContext);
static void (* volatile GH_glXFreeContextEXT)(Display *, GLXContext);
static Bool (* volatile GH_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
static Bool (* volatile GH_glXMakeContextCurrent)(Display *, GLXDrawable, GLXDrawable, GLXContext);
static Bool (* volatile GH_glXMakeCurrentReadSGI)(Display *, GLXDrawable, GLXDrawable, GLXContext);

static void (* volatile GH_glDebugMessageCallback)(GLDEBUGPROC, const GLvoid*);
static void (* volatile GH_glDebugMessageCallbackARB)(GLDEBUGPROC, const GLvoid*);
static void (* volatile GH_glDebugMessageCallbackKHR)(GLDEBUGPROC, const GLvoid*);
static void (* volatile GH_glDebugMessageCallbackAMD)(GLDEBUGPROCAMD, GLvoid*);

static void (* volatile GH_glTexParameteri)(GLenum, GLenum, GLint);
static void (* volatile GH_glTexParameterf)(GLenum, GLenum, GLfloat);
static void (* volatile GH_glGenerateMipmap)(GLenum);
static void (* volatile GH_glTexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (* volatile GH_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid * pixels);
static void (* volatile GH_glTexImage2D)(GLenum target, GLint level,  GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid * data);

/* function pointers we just might qeury */
static void (* volatile GH_glFlush)(void);
static void (* volatile GH_glFinish)(void);

static GLXFBConfig* (* volatile GH_glXGetFBConfigs)(Display*, int, int*);
static int (* volatile GH_glXGetFBConfigAttrib)(Display*, GLXFBConfig, int, int*);
static int (* volatile GH_XFree)(void*);

#ifdef GH_CONTEXT_TRACKING
/* OpenGL extension functions we might query */
static PFNGLGENQUERIESPROC GH_glGenQueries=NULL;
static PFNGLDELETEQUERIESPROC GH_glDeleteQueries=NULL;
static PFNGLGETINTEGER64VPROC GH_glGetInteger64v=NULL;
static PFNGLQUERYCOUNTERPROC GH_glQueryCounter=NULL;
static PFNGLGETQUERYOBJECTUI64VPROC GH_glGetQueryObjectui64v=NULL;
static PFNGLFENCESYNCPROC GH_glFenceSync=NULL;
static PFNGLDELETESYNCPROC GH_glDeleteSync=NULL;
static PFNGLCLIENTWAITSYNCPROC GH_glClientWaitSync=NULL;
#endif /* GH_CONTEXT_TRACKING */

/* Resolve an unintercepted symbol via the original dlsym() */
static void *GH_dlsym_next(const char *name)
{
	if (GH_dlsym) {
		return GH_dlsym(RTLD_NEXT, name);
	}
	GH_verbose(GH_MSG_WARNING, "failed to dynamically query '%s' because I don't have a dlsym\n", name);
	return NULL;
}

/* Wrapper funtcion to query the original dlsym() function avoiding
 * recursively calls to the interceptor dlsym() below */
static void GH_dlsym_internal_dlsym()
{
	static const char *dlsymname = "dlsym";
	static const char *dlvsymname = "dlvsym";
	DLSYM_PROC_T orig_dlsym = NULL;
	if (GH_dlsym == NULL) {
		GH_dlsym = GH_dlsym_internal_next(dlsymname);
		orig_dlsym = GH_dlsym;
		if (GH_dlsym) {
			void *ptr;
			GH_verbose(GH_MSG_DEBUG_INTERCEPTION,"INTERNAL: (%s) = %p, ours is %p\n",dlsymname,GH_dlsym,dlsym);
			ptr = GH_dlsym_next(dlsymname);
			if (ptr != (void*)GH_dlsym) {
				if (ptr) {
					if (get_envi("GH_ALLOW_DLSYM_REDIRECTION", 1)) {
						GH_verbose(GH_MSG_DEBUG_INTERCEPTION,"INTERNAL: (%s) = %p intercepted to %p\n",dlsymname,GH_dlsym,ptr);
						GH_dlsym = ptr;
					} else {
						GH_verbose(GH_MSG_WARNING, "INTERNAL: (%s) = %p would be intercepted to %p but ignoring it\n",dlsymname,GH_dlsym,ptr);
					}
				} else {
					GH_verbose(GH_MSG_WARNING,"INTERNAL: (%s) would be intercepted to NULL, ignoring it\n",dlsymname);
				}
			}
		} else {
			GH_verbose(GH_MSG_WARNING, "failed to dynamically query '%s'\n", dlsymname);
		}
	}

	/* use the original dlsym, not a potentially redirected one */
	if (orig_dlsym && (GH_dlvsym == NULL)) {
		Dl_info info;
		memset(&info,0,sizeof(info));
		GH_dlvsym = orig_dlsym(RTLD_NEXT,dlvsymname);
	}
}

/* Resolve an unintercepted symbol via the original dlsym(),
 * special libGL variant: if not found, dymically try to
 * load libGL.so */
static void *GH_dlsym_gl(const char *name)
{
	static void *libgl_handle=NULL;
	static int try_load_libgl=1;

	void *ptr=GH_dlsym(RTLD_NEXT, name);
	if (!ptr) {
		if (try_load_libgl && !libgl_handle) {
			const char *libname=get_envs("GH_LIBGL_FILE", "libGL.so");
			if (libname[0]) {
				GH_verbose(GH_MSG_DEBUG, "trying to load libGL manually: '%s'\n", libname);
				libgl_handle=dlopen(libname, RTLD_GLOBAL | RTLD_LAZY);
				if (!libgl_handle) {
					GH_verbose(GH_MSG_WARNING, "failed to load '%s' manually\n", libname);
					/* give up on loading libGL */
					try_load_libgl=0;
				}
			} else {
				try_load_libgl=0;
			}
		}
		if (libgl_handle) {
			GH_verbose(GH_MSG_DEBUG, "trying to find '%s' in manually loaded libGL\n", name);
			ptr=GH_dlsym(libgl_handle, name);
		}
	}
	return ptr;
}

/* helper macro: query the symbol pointer if it is NULL
 * handle the locking */
#define GH_GET_PTR(func) \
	pthread_mutex_lock(&GH_fptr_mutex); \
	if(GH_ ##func == NULL) \
		GH_ ##func = GH_dlsym_next(#func);\
	pthread_mutex_unlock(&GH_fptr_mutex)

/* helper macro: query the symbol pointer if it is NULL
 * handle the locking, special libGL variant */
#define GH_GET_PTR_GL(func) \
	pthread_mutex_lock(&GH_fptr_mutex); \
	if(GH_ ##func == NULL) \
		GH_ ##func = GH_dlsym_gl(#func);\
	pthread_mutex_unlock(&GH_fptr_mutex)

#ifdef GH_CONTEXT_TRACKING

/* try to get an OpenGL function */
static void *
GH_get_gl_proc(const char *name)
{
	static void *proc;

	/* try glXGetProcAddressARB first */
	GH_GET_PTR(glXGetProcAddressARB);
	if (GH_glXGetProcAddressARB && (proc=GH_glXGetProcAddressARB(name)) )
		return proc;

	/* try glXGetProcAddress as second chance */
	GH_GET_PTR(glXGetProcAddress);
	if (GH_glXGetProcAddress && (proc=GH_glXGetProcAddress(name)) )
		return proc;

	/* try dlsym as last resort */
	return GH_dlsym_gl(name);
}

#define GH_GET_GL_PROC(func) \
	pthread_mutex_lock(&GH_fptr_mutex); \
	if ( (GH_ ##func == NULL)) { \
		void *ptr; \
		pthread_mutex_unlock(&GH_fptr_mutex); \
		ptr = GH_get_gl_proc(#func); \
		GH_verbose(GH_MSG_DEBUG,"queried internal GL %s: %p\n", \
			#func, ptr); \
		pthread_mutex_lock(&GH_fptr_mutex); \
		GH_ ##func = ptr; \
	} \
	pthread_mutex_unlock(&GH_fptr_mutex);

#define GH_GET_GL_PROC_OR_FAIL(func, level, fail_code) \
	GH_GET_GL_PROC(func); \
	if (GH_ ##func == NULL) { \
		GH_verbose(level, "%s not available!\n", #func); \
		return fail_code; \
	} \
	(void)0

/***************************************************************************
 * LATENCY LIMITER                                                         *
 ***************************************************************************/


/* we support different modes for the frame time measurements */
typedef enum {
	GH_LATENCY_NOP=-2,		/* do nothing */
	GH_LATENCY_FINISH_AFTER,	/* force a finish after buffer swaps */
	GH_LATENCY_FINISH_BEFORE,	/* force a finish before buffer swaps */
	/* values above 0 indicate use of GL_ARB_sync to limit to n frames */
} GH_latency_mode;

typedef struct {
	int latency;
	GLsync *sync_object;
	unsigned int cur_pos;
	unsigned int flags;
	GLuint64 gl_wait_timeout;
	GLuint64 gl_wait_interval;
	useconds_t self_wait_interval;
} GH_latency;

/* latency flags */
#define GH_LATENCY_FLAG_MANUAL_WAIT	0x1

static int
latency_gl_init()
{
	GH_GET_GL_PROC_OR_FAIL(glFenceSync, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glDeleteSync, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glClientWaitSync, GH_MSG_WARNING, -1);
	return 0;
}

static void
latency_init(GH_latency *lat, int latency, int manual_wait, unsigned gl_wait_timeout_usecs, unsigned int gl_wait_interval_usecs, unsigned int self_wait_interval_usecs)
{
	lat->latency=latency;
	lat->sync_object=NULL;
	lat->cur_pos=0;
	lat->flags=0;
	lat->gl_wait_timeout=(GLuint64)gl_wait_timeout_usecs * (GLuint64)1000;
	lat->gl_wait_interval=(GLuint64)gl_wait_interval_usecs * (GLuint64)1000;
	lat->self_wait_interval=(useconds_t)self_wait_interval_usecs;

	if (manual_wait > 0) {
		lat->flags |= GH_LATENCY_FLAG_MANUAL_WAIT;
	} else if (manual_wait < 0) {
		if ((gl_wait_interval_usecs > 0) || (self_wait_interval_usecs > 0))  {
			lat->flags |= GH_LATENCY_FLAG_MANUAL_WAIT;
		}
	}

	if (latency != GH_LATENCY_NOP) {
		GH_verbose(GH_MSG_INFO, "setting up latency limiter mode %d\n", latency);
	}

	if (latency > 0) {
		if (latency_gl_init()) {
			lat->latency=GH_LATENCY_FINISH_BEFORE;
			GH_verbose(GH_MSG_WARNING, "GPU sync not available, using latency mode %d\n",
					GH_LATENCY_FINISH_BEFORE);
		}
	}

	if (lat->latency > 0) {
		unsigned int cnt=(unsigned)lat->latency;
		lat->sync_object=malloc(sizeof(*lat->sync_object) * cnt);
		if (!lat->sync_object) {
			lat->latency=GH_LATENCY_FINISH_BEFORE;
			GH_verbose(GH_MSG_WARNING, "out of memory for sync objects, using latency mode %d\n");
		} else {
			unsigned int i;
			for (i=0; i<cnt; i++) {
				lat->sync_object[i]=NULL;
			}
			GH_verbose(GH_MSG_DEBUG, "enabling latency limiter: %d\n",lat->latency);
			if (lat->flags & GH_LATENCY_FLAG_MANUAL_WAIT) {
				GH_verbose(GH_MSG_INFO, "latency limiter with manual waits GL: %u usecs + self: %u usecs\n",
						gl_wait_interval_usecs, self_wait_interval_usecs);
			} else {
				GH_verbose(GH_MSG_INFO, "latency limiter with timeout: %u usecs\n",
						gl_wait_timeout_usecs);
			}
		}
	}
}

static void
latency_destroy(GH_latency *lat)
{
	if (lat) {
		if (lat->sync_object && lat->latency > 0) {
			unsigned int i;
			for (i=0; i<(unsigned)lat->latency; i++) {
				if (lat->sync_object[i]) {
					GH_glDeleteSync(lat->sync_object[i]);
				}
			}
		}
	}
}

static void
latency_before_swap(GH_latency *lat)
{
	GLsync sync;

	switch(lat->latency) {
		case GH_LATENCY_NOP:
		case GH_LATENCY_FINISH_AFTER:
			(void)lat;
			break;
		case GH_LATENCY_FINISH_BEFORE:
			GH_glFinish();
			break;
		default:
			if ( (sync=lat->sync_object[lat->cur_pos]) ) {
				if (lat->flags & GH_LATENCY_FLAG_MANUAL_WAIT) {
					/* check for the fence in a loop */
					while(GH_glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, lat->gl_wait_interval) == GL_TIMEOUT_EXPIRED) {
						if (lat->self_wait_interval) {
							usleep(lat->self_wait_interval);
						}
					}
				} else {
					/* just wait for the fence */
					GH_glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, lat->gl_wait_timeout);
					/* NOTE: we do not care about the result */
				}
			}
	}
}

static void
latency_after_swap(GH_latency *lat)
{
	switch(lat->latency) {
		case GH_LATENCY_NOP:
		case GH_LATENCY_FINISH_BEFORE:
			(void)lat;
			break;
		case GH_LATENCY_FINISH_AFTER:
			GH_glFinish();
			break;
		default:
			if ( (lat->sync_object[lat->cur_pos]) ) {
				GH_glDeleteSync(lat->sync_object[lat->cur_pos]);
			}
			lat->sync_object[lat->cur_pos]=GH_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			if (++lat->cur_pos == (unsigned)lat->latency) {
				lat->cur_pos=0;
			}
	}
}

/***************************************************************************
 * FRAME TIMING MEASUREMENTS                                               *
 ***************************************************************************/

/* we support different modes for the frame time measurements */
typedef enum {
	GH_FRAMETIME_NONE=0,	/* do not measure any frame times */
	GH_FRAMETIME_CPU,	/* measure only on CPU */
	GH_FRAMETIME_CPU_GPU,	/* measure both on CPU and GPU */
} GH_frametime_mode;

/* a single timestamp */
typedef struct {
	struct timespec timestamp_cpu;	/* CPU timestamp */
	GLuint64 timestamp_gl;		/* OpenGL timestamp (client side) */
	GLuint query_object;		/* the OpenGL timer query object */
} GH_timestamp;

/* the _result_ of the measurement */
typedef struct {
	uint64_t cpu;
	uint64_t gl;
	uint64_t gpu;
} GH_frametime;

/* the complete state needed for frametime measurements */
typedef struct {
	GH_frametime_mode mode;		/* the mode we are in */
	unsigned int delay;		/* number of frames for delay */
	unsigned int num_timestamps;	/* number of timestamps per frame */
	unsigned int num_results;	/* number of frames to store results */
	GH_timestamp *timestamp;	/* the array of timestamps, used as ring buffer */
	unsigned int cur_pos;		/* the current frame in the delay ring buffer */
	GH_frametime *frametime;	/* the array of frametimes */
	unsigned int cur_result;	/* the current result index */
	unsigned int frame;		/* the current frame */
	FILE *dump;			/* the stream to dump the results to */
} GH_frametimes;

/* the probes we take each frame */
typedef enum {
	GH_FRAMETIME_BEFORE_SWAPBUFFERS=0,
	GH_FRAMETIME_AFTER_SWAPBUFFERS,
	GH_FRAMETIME_COUNT
} GH_frametime_probe;

static void
timestamp_init(GH_timestamp *ts)
{
	ts->query_object=0;
	ts->timestamp_gl=0;
	ts->timestamp_cpu.tv_sec=0;
	ts->timestamp_cpu.tv_nsec=0;
}

static void
timestamp_cleanup(GH_timestamp *ts)
{
	if (ts->query_object && GH_glDeleteQueries) {
		GH_glDeleteQueries(1, &ts->query_object);
		ts->query_object=0;
	}
}

static void
timestamp_set(GH_timestamp *ts, GH_frametime *rs, GH_frametime_mode mode)
{
	/* CPU */
	/* collect previous result */
	rs->cpu=(uint64_t)ts->timestamp_cpu.tv_sec * (uint64_t)1000000000UL
		+ (uint64_t)ts->timestamp_cpu.tv_nsec;
	/* new query */
	clock_gettime(CLOCK_REALTIME, &ts->timestamp_cpu);

	/* GPU */
	if (mode >= GH_FRAMETIME_CPU_GPU) {
		/* collect previous result */
		if (ts->query_object) {
			GLuint64 value;
			GH_glGetQueryObjectui64v(ts->query_object, GL_QUERY_RESULT, &value);
			rs->gpu=(uint64_t)value;
		} else {
			GH_glGenQueries(1, &ts->query_object);
		}
		rs->gl=(uint64_t)(ts->timestamp_gl);
		/* new query */
		GH_glQueryCounter(ts->query_object, GL_TIMESTAMP);
		GH_glGetInteger64v(GL_TIMESTAMP, (GLint64*)&ts->timestamp_gl);
	}
}

static void
frametime_init(GH_frametime *rs)
{
	rs->cpu = 0;
	rs->gl  = 0;
	rs->gpu = 0;
}

static int
frametimes_gl_init()
{
	GH_GET_GL_PROC_OR_FAIL(glGenQueries, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glDeleteQueries, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glGetInteger64v, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glQueryCounter, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glGetQueryObjectui64v, GH_MSG_WARNING, -1);
	return 0;
}

static void
frametimes_init(GH_frametimes *ft, GH_frametime_mode mode, unsigned int delay, unsigned int num_timestamps, unsigned int num_results, unsigned int ctx_num)
{
	ft->cur_pos=0;
	ft->cur_result=0;
	ft->frame=0;
	ft->dump=NULL;

	if (mode >= GH_FRAMETIME_CPU_GPU) {
		if (frametimes_gl_init()) {
			GH_verbose(GH_MSG_WARNING, "GPU timer queries not available, using CPU only\n");
			mode = GH_FRAMETIME_CPU;
		}
	}

	if (mode && delay && num_timestamps && num_results) {
		ft->mode=mode;
		ft->delay=delay;
		ft->num_timestamps=num_timestamps;
		ft->num_results=num_results;
		if ((ft->frametime=malloc(sizeof(*ft->frametime) * (num_results+1) * num_timestamps))) {
			if ((ft->timestamp=malloc(sizeof(*ft->timestamp) * delay * num_timestamps))) {
				unsigned int i;
				GH_verbose(GH_MSG_DEBUG, "enabling frametime measurements mode %d,  %u x %u timestamps\n",
						(int)mode, delay, num_timestamps);
				for (i=0; i<delay * num_timestamps; i++) {
					timestamp_init(&ft->timestamp[i]);
				}
			} else {
				GH_verbose(GH_MSG_WARNING, "failed to allocate memory for %u x %u timestamps, "
						"disbaling timestamps\n",
						delay, num_timestamps);
				mode=GH_FRAMETIME_NONE;
			}
		} else {
			GH_verbose(GH_MSG_WARNING, "failed to allocate memory for %u x %u frametime results, "
					"disbaling timestamps\n",
					num_results, num_timestamps);
			mode=GH_FRAMETIME_NONE;
		}
	}

	if (!mode || !delay || !num_timestamps || !num_results) {
		ft->mode=GH_FRAMETIME_NONE;
		ft->delay=0;
		ft->num_timestamps=0;
		ft->num_results=0;
		ft->timestamp=NULL;
		ft->frametime=NULL;
	}

	if (ft->mode) {
		const char *file=get_envs("GH_FRAMETIME_FILE","glx_hook_frametimes-ctx%c.csv");
		if (file) {
			char buf[PATH_MAX];
			parse_name(buf, sizeof(buf), file, ctx_num);
			ft->dump=fopen(buf,"wt");
		}
		if (!ft->dump) {
			ft->dump=stderr;
		}
	}
}

static void
frametimes_init_base(GH_frametimes *ft)
{
	/* get the base timestamp */
	if (ft->mode > GH_FRAMETIME_NONE) {
		GH_timestamp base;
		unsigned int i;
		GH_frametime *last=&ft->frametime[ft->num_results * ft->num_timestamps];

		timestamp_init(&base);
		timestamp_set(&base, &last[0], ft->mode);
		timestamp_set(&base, &last[0], ft->mode);
		for (i=1; i<ft->num_timestamps; i++) {
			last[i]=last[0];
		}
		timestamp_cleanup(&base);
	}
}

static void
frametimes_dump_diff(const GH_frametimes *ft, uint64_t val, uint64_t base)
{
	fprintf(ft->dump, "\t%llu", (unsigned long long) (val-base));
}

static void
frametimes_dump_result(const GH_frametimes *ft, const GH_frametime *rs, const GH_frametime *base)
{
	frametimes_dump_diff(ft, rs->cpu, base->cpu);
	frametimes_dump_diff(ft, rs->gpu, base->gpu);
	frametimes_dump_diff(ft, rs->gpu, rs->gl);
}

static void
frametimes_dump_results(const GH_frametimes *ft, const GH_frametime *rs, const GH_frametime *prev)
{
	unsigned int i;
	for (i=0; i<ft->num_timestamps; i++) {
		frametimes_dump_result(ft, &rs[i], &prev[GH_FRAMETIME_AFTER_SWAPBUFFERS]);
	}
}

static void
frametimes_flush(GH_frametimes *ft)
{
	unsigned int i;
	GH_frametime *last;
	const GH_frametime *cur,*prev=&ft->frametime[ft->num_results * ft->num_timestamps];

	if (ft->cur_result == 0) {
		return;
	}

	GH_verbose(GH_MSG_DEBUG, "frametimes: dumping results of %u frames\n", ft->cur_result);
	for (i=0; i<ft->cur_result; i++) {
		unsigned int frame=ft->frame - ft->cur_result + i;
		if (frame >= ft->delay) {
			fprintf(ft->dump, "%u", frame - ft->delay);
			cur=&ft->frametime[i * ft->num_timestamps];
			frametimes_dump_results(ft, cur, prev);
			prev=cur;
			fputc('\n', ft->dump);
		}
	}
	fflush(ft->dump);
	/* copy the last result */
	last=&ft->frametime[ft->num_results * ft->num_timestamps];
	cur=&ft->frametime[(ft->cur_result-1) * ft->num_timestamps];
	for (i=0; i<ft->num_timestamps; i++) {
		last[i]=cur[i];
	}


	ft->cur_result=0;
}

static void
frametimes_destroy(GH_frametimes *ft)
{
	if (ft) {
		unsigned int cnt=ft->delay * ft->num_timestamps;
		unsigned int i;

		frametimes_flush(ft);
		if (ft->dump != NULL && ft->dump != stdout && ft->dump != stderr) {
			fclose(ft->dump);
		}
		for (i=0; i<cnt; i++) {
			timestamp_cleanup(&ft->timestamp[i]);
		}
		free(ft->timestamp);
		free(ft->frametime);
	}
}

static void
frametimes_before_swap(GH_frametimes *ft)
{
	unsigned int ts_idx;
	unsigned int rs_idx;

	if (ft->mode == GH_FRAMETIME_NONE)
		return;
	ts_idx=ft->cur_pos * ft->num_timestamps + GH_FRAMETIME_BEFORE_SWAPBUFFERS;
	rs_idx=ft->cur_result * ft->num_timestamps + GH_FRAMETIME_BEFORE_SWAPBUFFERS;
	timestamp_set(&ft->timestamp[ts_idx], &ft->frametime[rs_idx], ft->mode);
}

static void
frametimes_finish_frame(GH_frametimes *ft)
{
	if (++ft->cur_pos == ft->delay) {
		ft->cur_pos=0;
	}
	++ft->frame;
	if (++ft->cur_result >= ft->num_results) {
		frametimes_flush(ft);
	}
}

static void
frametimes_after_swap(GH_frametimes *ft)
{
	unsigned int ts_idx;
	unsigned int rs_idx;

	if (ft->mode == GH_FRAMETIME_NONE)
		return;
	ts_idx=ft->cur_pos * ft->num_timestamps + GH_FRAMETIME_AFTER_SWAPBUFFERS;
	rs_idx=ft->cur_result * ft->num_timestamps + GH_FRAMETIME_AFTER_SWAPBUFFERS;
	timestamp_set(&ft->timestamp[ts_idx], &ft->frametime[rs_idx], ft->mode);
	frametimes_finish_frame(ft);
}

/***************************************************************************
 * SWAPBUFFER OMISSION (very experimental)                                 *
 ***************************************************************************/

#define GH_SWAP_OMISSION_FRAMES_MAX 16

typedef struct {
	int swapbuffers;
	int swapbuffer_cnt;
	int latency_mode;
	int flush_mode;
	int measure_mode;
	int limits[2];
	uint64_t min_swap_time;
	GH_timestamp prev_frame_ts[GH_SWAP_OMISSION_FRAMES_MAX][2];
	GH_frametime prev_frames[GH_SWAP_OMISSION_FRAMES_MAX][2];
	int prev_intervals[GH_SWAP_OMISSION_FRAMES_MAX];
	unsigned int cur_pos;
	unsigned int measure_frames_tot;
	unsigned int measure_frames_avg;
} GH_swapbuffer_omission_t;

static void
swapbuffer_omission_init(GH_swapbuffer_omission_t *swo)
{
	int i;
	int min_swap_usecs = get_envi("GH_MIN_SWAP_USECS",0);
	swo->swapbuffers=get_envi("GH_SWAPBUFFERS",0);
	swo->min_swap_time = 0;
	if (min_swap_usecs > 0) {
		swo->swapbuffers = 1;
		swo->min_swap_time = (uint64_t)min_swap_usecs * (uint64_t)1000UL;
	}
	swo->latency_mode = get_envi("GH_SWAP_OMISSION_LATENCY", 0);
	swo->flush_mode = get_envi("GH_SWAP_OMISSION_FLUSH", 1);
	swo->measure_mode = get_envi("GH_SWAP_OMISSION_MEASURE", 3);
	swo->limits[0] = get_envi("GH_SWAP_OMISSION_MIN",1);
	swo->limits[1] = get_envi("GH_SWAP_OMISSION_MAX",4);
	if (swo->limits[0] < 1) {
		swo->limits[0] = 1;
	}
	if (swo->limits[1] < swo->limits[0]) {
		swo->limits[1] = swo->limits[0];
	}
	swo->swapbuffer_cnt=0;
	swo->cur_pos = 0;
	swo->measure_frames_tot = get_envui("GH_SWAP_OMISSION_MEASURE_TOT", 6U);
	swo->measure_frames_avg = get_envui("GH_SWAP_OMISSION_MEASURE_AVG", 4U);
	if (swo->measure_frames_tot > GH_SWAP_OMISSION_FRAMES_MAX) {
		swo->measure_frames_tot = GH_SWAP_OMISSION_FRAMES_MAX;
	} else if (swo->measure_frames_tot < 2) {
		swo->measure_frames_tot = 2;
	}
	if (swo->measure_frames_avg >= swo->measure_frames_tot) {
		swo->measure_frames_avg = swo->measure_frames_tot - 1U;
	}
	if (swo->measure_frames_avg < 1) {
		swo->measure_frames_avg = 1;
	}

	for (i=0; i<GH_SWAP_OMISSION_FRAMES_MAX; i++) {
		timestamp_init(&swo->prev_frame_ts[i][0]);
		timestamp_init(&swo->prev_frame_ts[i][1]);
		frametime_init(&swo->prev_frames[i][0]);
		frametime_init(&swo->prev_frames[i][1]);
		swo->prev_intervals[i] = 1;
	}
}

static int
swapbuffer_omission_init_gl_funcs(void)
{
	GH_GET_GL_PROC_OR_FAIL(glGenQueries, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glDeleteQueries, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glGetInteger64v, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glQueryCounter, GH_MSG_WARNING, -1);
	GH_GET_GL_PROC_OR_FAIL(glGetQueryObjectui64v, GH_MSG_WARNING, -1);
	return 0;
}

static void
swapbuffer_omission_init_gl(GH_swapbuffer_omission_t *swo)
{
	if (swo->min_swap_time > 0) {
		if (swapbuffer_omission_init_gl_funcs()) {
			GH_verbose(GH_MSG_WARNING,"adaptive swapbuffer omission not availabe without timer query, disabling it");
			swo->min_swap_time = 0;
			swo->swapbuffers = get_envi("GH_SWAPBUFFERS",0);;
		}
	}
}

static int
swapbuffer_omission_do_swap(GH_swapbuffer_omission_t *swo)
{
	int do_swap;
	if (swo->min_swap_time > 0) {
		unsigned int i,idx;
		uint64_t cpu = 0;
		uint64_t gpu = 0;
		uint64_t val;
		timestamp_set(&swo->prev_frame_ts[swo->cur_pos][1],&swo->prev_frames[swo->cur_pos][1], GH_FRAMETIME_CPU_GPU);
		if (++swo->cur_pos >= swo->measure_frames_tot) {
			swo->cur_pos = 0;
		}
		idx = swo->cur_pos;

		for (i=0; i<swo->measure_frames_avg; i++) {
			cpu += (swo->prev_frames[idx][1].cpu - swo->prev_frames[idx][0].cpu);
			gpu += (swo->prev_frames[idx][1].gpu - swo->prev_frames[idx][0].gpu);
			if (++idx >= swo->measure_frames_tot) {
				idx = 0;
			}
		}
		cpu /= swo->measure_frames_avg;
		gpu /= swo->measure_frames_avg;
		switch (swo->measure_mode) {
			case 1:
				val=cpu;
				break;
			case 2:
				val=gpu;
				break;
			default:
				val = (cpu>gpu)?cpu:gpu;
		}
		if (val < 1000) {
			val = 1000;
		}
		int interval  = (int)(swo->min_swap_time/val);
		if (interval < swo->limits[0]) {
			interval = swo->limits[0];
		}
		if (interval > swo->limits[1]) {
			interval = swo->limits[1];
		}
		swo->prev_intervals[swo->cur_pos]=interval;
		idx = swo->cur_pos + swo->measure_frames_tot - swo->measure_frames_avg + 1;
		for (i=0; i<(swo->measure_frames_avg-1); i++) {
			if (idx >= swo->measure_frames_tot) {
				idx -= swo->measure_frames_tot;
			}
			interval += swo->prev_intervals[idx++];
		}
		swo->swapbuffers = interval / swo->measure_frames_avg;

		/*printf("XXX %d %d cpu: %lu, gpu: %lu\n", swo->swapbuffers, interval, cpu, gpu);*/
	}
	do_swap = (++swo->swapbuffer_cnt >= swo->swapbuffers);
	return do_swap;
}

static void
swapbuffer_omission_swap_skipped(GH_swapbuffer_omission_t *swo)
{
	switch (swo->flush_mode) {
		case 1:
			GH_glFlush();
			break;
		case 2:
			GH_glFinish();
			break;
		default:
			(void)0; /* nop */
	}
}

static void
swapbuffer_omission_swap_finished(GH_swapbuffer_omission_t *swo, int did_swap)
{
	if (did_swap) {
		swo->swapbuffer_cnt = 0;
	}
	if (swo->min_swap_time > 0) {
		timestamp_set(&swo->prev_frame_ts[swo->cur_pos][0],&swo->prev_frames[swo->cur_pos][0], GH_FRAMETIME_CPU_GPU);
	}
}

static void
swapbuffer_omission_destroy(GH_swapbuffer_omission_t *swo)
{
	int i;
	for (i=0; i<GH_SWAP_OMISSION_FRAMES_MAX; i++) {
		timestamp_cleanup(&swo->prev_frame_ts[i][0]);
		timestamp_cleanup(&swo->prev_frame_ts[i][1]);
	}
}

/***************************************************************************
 * GL context tracking                                                     *
 ***************************************************************************/

typedef struct gl_context_ceation_opts_s {
	pthread_mutex_t mutex;
	unsigned int flags;
	int force_version[2];
	int force_version_min[2];
	int force_version_max[2];
	int force_flags_on;
	int force_flags_off;
	int force_profile_on;
	int force_profile_off;
	int force_no_error;
} gl_context_creation_opts_t;

/* flag bits */
#define GH_GLCTX_CREATE_INITIALIZED	0x1
#define GH_GLCTX_COMPAT_IF_LEGACY	0x2

typedef struct gl_context_s {
	GLXContext ctx;
	GLXDrawable draw;
	GLXDrawable read;
	unsigned int flags;
	int inject_swapinterval;
	unsigned int num;
	struct gl_context_s *next;
	GH_frametimes frametimes;
	GH_latency latency;
	GH_swapbuffer_omission_t swapbuffer_omission;
	useconds_t swap_sleep_usecs;
	GLDEBUGPROC original_debug_callback;
	GLDEBUGPROCAMD original_debug_callback_AMD;
	const GLvoid* original_debug_callback_user_ptr;
	GLvoid* original_debug_callback_AMD_user_ptr;
} gl_context_t;

/* flag bits */
#define GH_GL_CURRENT		0x1
#define GH_GL_NEVER_CURRENT	0x2
#define GH_GL_INTERCEPT_DEBUG	0x4
#define GH_GL_INJECT_DEBUG	0x8

static gl_context_creation_opts_t ctx_creation_opts = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.flags = 0,
	.force_version = {-1, -1},
	.force_version_min = {-1, -1},
	.force_version_max = {-1, -1},
	.force_flags_on = 0,
	.force_flags_off = 0,
	.force_profile_on = 0,
	.force_profile_off = 0,
	.force_no_error = -1,
};
static gl_context_t * volatile ctx_list=NULL;
static volatile int ctx_counter=0;
static pthread_mutex_t ctx_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t ctx_current;

/* forward declarations */
static void APIENTRY
GH_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
		  GLsizei length, const GLchar *message, const GLvoid* userParam);
static void APIENTRY
GH_debug_callback_AMD(GLuint id, GLenum category, GLenum severity,
		  GLsizei length, const GLchar *message, GLvoid* userParam);

static void
add_ctx(gl_context_t *glc)
{
	pthread_mutex_lock(&ctx_mutex);
	glc->next=ctx_list;
	ctx_list=glc;
	pthread_mutex_unlock(&ctx_mutex);
}

static gl_context_t *
create_ctx(GLXContext ctx, unsigned int num)
{
	gl_context_t *glc;

	glc=malloc(sizeof(*glc));
	if (glc) {
		glc->ctx=ctx;
		glc->draw=None;
		glc->read=None;
		glc->inject_swapinterval=GH_SWAP_DONT_SET;
		glc->flags=GH_GL_NEVER_CURRENT;
		glc->num=num;
		glc->original_debug_callback=(GLDEBUGPROC)NULL;
		glc->original_debug_callback_AMD=(GLDEBUGPROCAMD)NULL;
		glc->original_debug_callback_user_ptr=NULL;
		glc->original_debug_callback_AMD_user_ptr=NULL;

		glc->swap_sleep_usecs=0;
		frametimes_init(&glc->frametimes, GH_FRAMETIME_NONE, 0, 0, 0, num);
		latency_init(&glc->latency, GH_LATENCY_NOP, 0, 1000000, 0, 0);
		swapbuffer_omission_init(&glc->swapbuffer_omission);
	}
	return glc;
}

static void
destroy_ctx(gl_context_t *glc)
{
	if (glc) {
		frametimes_destroy(&glc->frametimes);
		latency_destroy(&glc->latency);
		swapbuffer_omission_destroy(&glc->swapbuffer_omission);
		free(glc);
	}
}

static gl_context_t *
find_ctx(GLXContext ctx)
{
	gl_context_t *glc;

	pthread_mutex_lock(&ctx_mutex);
	for (glc=ctx_list; glc; glc=glc->next)
		if (glc->ctx == ctx)
			break;
	pthread_mutex_unlock(&ctx_mutex);
	return glc;
}

static void
remove_ctx(GLXContext ctx)
{
	gl_context_t *glc,*prev=NULL;

	pthread_mutex_lock(&ctx_mutex);
	for (glc=ctx_list; glc; glc=glc->next) {
		if (glc->ctx == ctx)
			break;
		prev=glc;
	}
	if (glc) {
		if (prev) 
			prev->next=glc->next;
		else
			ctx_list=glc->next;
	}
	pthread_mutex_unlock(&ctx_mutex);
	if (glc) {
		destroy_ctx(glc);
	}
}

static void
read_config(gl_context_t *glc)
{
	glc->inject_swapinterval=get_envi("GH_INJECT_SWAPINTERVAL", GH_SWAP_DONT_SET);
	if (get_envi("GH_GL_DEBUG_OUTPUT",0)) {
		glc->flags |= GH_GL_INTERCEPT_DEBUG;
	}
	if (get_envi("GH_GL_INJECT_DEBUG_OUTPUT",0)) {
		glc->flags |= GH_GL_INJECT_DEBUG;
	}
}

static void
create_context(GLXContext ctx)
{
	gl_context_t *glc;
	unsigned int ctx_num;

	GH_verbose(GH_MSG_DEBUG, "created ctx %p\n",ctx);

	pthread_mutex_lock(&ctx_mutex);
	ctx_num=ctx_counter++;
	if (!ctx_num) {
		pthread_key_create(&ctx_current, NULL);
		pthread_setspecific(ctx_current, NULL);
		/* query the function pointers for the standard functions
		 * which might be often called ... */
		GH_GET_PTR_GL(glXSwapBuffers);
		GH_GET_PTR_GL(glXMakeCurrent);
		GH_GET_PTR_GL(glXMakeContextCurrent);
		GH_GET_PTR_GL(glXMakeCurrentReadSGI);
		GH_GET_PTR_GL(glFlush);
		GH_GET_PTR_GL(glFinish);
	}
	pthread_mutex_unlock(&ctx_mutex);
	
	glc=create_ctx(ctx, ctx_num);
	if (glc) {
		read_config(glc);
		/* add to our list */
		add_ctx(glc);
	} else {
		GH_verbose(GH_MSG_ERROR, "out of memory\n");
	}

}

static void
destroy_context(GLXContext ctx)
{
	GH_verbose(GH_MSG_INFO, "destroyed ctx %p\n",ctx);
	remove_ctx(ctx);
}

static void
make_current(GLXContext ctx, Display *dpy, GLXDrawable draw, GLXDrawable read)
{
	gl_context_t *glc;

	glc=(gl_context_t*)pthread_getspecific(ctx_current);
	if (glc) {
		/* old context */
		glc->flags &= ~GH_GL_CURRENT;
		GH_verbose(GH_MSG_DEBUG, "unbound context %p\n",glc->ctx);
	}
	
	if (ctx) {
		glc=find_ctx(ctx);

		if (glc == NULL) {
			GH_verbose(GH_MSG_WARNING, "app tried to make current non-existing context %p\n",ctx);
		} else {
			glc->draw=draw;
			glc->read=read;
			glc->flags |= GH_GL_CURRENT;
			GH_verbose(GH_MSG_DEBUG, "made current context %p\n",ctx);
			if (glc->flags & GH_GL_NEVER_CURRENT) {
				unsigned int ft_delay=get_envui("GH_FRAMETIME_DELAY", 10);
				unsigned int ft_frames=get_envui("GH_FRAMETIME_FRAMES", 1000);
				GH_frametime_mode ft_mode=(GH_frametime_mode)get_envi("GH_FRAMETIME", (int)GH_FRAMETIME_NONE);
				int latency=get_envi("GH_LATENCY", GH_LATENCY_NOP);
				int latency_manual_wait=get_envi("GH_LATENCY_MANUAL_WAIT", -1);
				unsigned int latency_gl_wait_timeout=get_envui("GH_LATENCY_GL_WAIT_TIMEOUT_USECS", 1000000);
				unsigned int latency_gl_wait_interval=get_envui("GH_LATENCY_GL_WAIT_USECS", 0);
				unsigned int latency_self_wait_interval=get_envui("GH_LATENCY_WAIT_USECS", 0);
				/* made current for the first time */
				glc->flags &= ~ GH_GL_NEVER_CURRENT;

				glc->swap_sleep_usecs=(useconds_t)get_envui("GH_SWAP_SLEEP_USECS",0);
				frametimes_init(&glc->frametimes, ft_mode, ft_delay, GH_FRAMETIME_COUNT, ft_frames, glc->num);
				frametimes_init_base(&glc->frametimes);
				latency_init(&glc->latency, latency, latency_manual_wait, latency_gl_wait_timeout, latency_gl_wait_interval, latency_self_wait_interval);
				swapbuffer_omission_init_gl(&glc->swapbuffer_omission);
				if (glc->inject_swapinterval != GH_SWAP_DONT_SET) {
					GH_GET_PTR_GL(glXSwapIntervalEXT);
					if (GH_glXSwapIntervalEXT) {
						GH_verbose(GH_MSG_INFO, "injecting swap interval: %d\n",
								glc->inject_swapinterval);
						GH_glXSwapIntervalEXT(dpy, glc->draw, glc->inject_swapinterval);
					} else {
						GH_GET_PTR_GL(glXSwapIntervalSGI);
						if (GH_glXSwapIntervalEXT) {
							GH_verbose(GH_MSG_INFO, "injecting swap interval: %d\n",
									glc->inject_swapinterval);
							GH_glXSwapIntervalSGI(glc->inject_swapinterval);
						}
					}
				}
				if (glc->flags & GH_GL_INJECT_DEBUG) {
					GH_GET_PTR_GL(glDebugMessageCallback);
					if (GH_glDebugMessageCallback) {
						GH_glDebugMessageCallback(GH_debug_callback, glc);
						GH_verbose(GH_MSG_INFO, "injecting debug callback [core]\n");
					} else {
						GH_GET_PTR_GL(glDebugMessageCallbackARB);
						if (GH_glDebugMessageCallbackARB) {
							GH_glDebugMessageCallbackARB(GH_debug_callback, glc);
							GH_verbose(GH_MSG_INFO, "injecting debug callback [ARB]\n");
						} else {
							GH_GET_PTR_GL(glDebugMessageCallbackKHR);
							if (GH_glDebugMessageCallbackKHR) {
								GH_glDebugMessageCallbackKHR(GH_debug_callback, glc);
								GH_verbose(GH_MSG_INFO, "injecting debug callback [KHR]\n");
							} else {
								GH_GET_PTR_GL(glDebugMessageCallbackAMD);
								if (GH_glDebugMessageCallbackAMD) {
									GH_glDebugMessageCallbackAMD(GH_debug_callback_AMD, glc);
									GH_verbose(GH_MSG_INFO, "injecting debug callback [AMD]\n");
								} else {
									GH_verbose(GH_MSG_WARNING, "failed ot iject debug message callback\n");
								}
							}
						}
					}
				}
			}
		}
	} else {
		glc=NULL;
	}

	pthread_setspecific(ctx_current, glc);
}

/****************************************************************************
 * GL DEBUG MESSAGES                                                        *
 ****************************************************************************/

/* Newer versions of the GL support the generation of human-readable messages
   for GL errors, performance warnings and hints. These messages are
   forwarded to a debug callback which has to be registered with the GL
   context. Debug output may only be available in special debug context... */

/* translate the debug message "source" enum to human-readable string */
static const char *
translateDebugSourceEnum(GLenum source)
{
	const char *s;

	switch (source) {
		case GL_DEBUG_SOURCE_API:
			s="API";
			break;
		case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
			s="window system";
			break;
		case GL_DEBUG_SOURCE_SHADER_COMPILER:
			s="shader compiler";
			break;
		case GL_DEBUG_SOURCE_THIRD_PARTY:
			s="3rd party";
			break;
		case GL_DEBUG_SOURCE_APPLICATION:
			s="application";
			break;
		case GL_DEBUG_SOURCE_OTHER:
			s="other";
			break;
		default:
			s="[UNKNOWN SOURCE]";
	}

	return s;
}

/* translate the debug message "category" enum to human-readable string */
static const char *
translateDebugCategoryEnum(GLenum cat)
{
	const char *s;

	switch (cat) {
		case GL_DEBUG_CATEGORY_API_ERROR_AMD:
			s="API error";
			break;
		case GL_DEBUG_CATEGORY_WINDOW_SYSTEM_AMD:
			s="window system";
			break;
		case GL_DEBUG_CATEGORY_DEPRECATION_AMD:
			s="deprecation";
			break;
		case GL_DEBUG_CATEGORY_UNDEFINED_BEHAVIOR_AMD:
			s="undefined behavior";
			break;
		case GL_DEBUG_CATEGORY_PERFORMANCE_AMD:
			s="performance";
			break;
		case GL_DEBUG_CATEGORY_SHADER_COMPILER_AMD:
			s="shader compiler";
			break;
		case GL_DEBUG_CATEGORY_APPLICATION_AMD:
			s="application";
			break;
		case GL_DEBUG_CATEGORY_OTHER_AMD:
			s="other";
			break;
		default:
			s="[UNKNOWN CATEGORY]";
	}

	return s;
}

/* translate the debug message "type" enum to human-readable string */
static const char *
translateDebugTypeEnum(GLenum type)
{
	const char *s;

	switch (type) {
		case GL_DEBUG_TYPE_ERROR:
			s="error";
			break;
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
			s="deprecated";
			break;
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
			s="undefined behavior";
			break;
		case GL_DEBUG_TYPE_PORTABILITY:
			s="portability";
			break;
		case GL_DEBUG_TYPE_PERFORMANCE:
			s="performance";
			break;
		case GL_DEBUG_TYPE_OTHER:
			s="other";
			break;
		default:
			s="[UNKNOWN TYPE]";
	}
	return s;
}

/* translate the debug message "xeverity" enum to human-readable string */
static const char *
translateDebugSeverityEnum(GLenum severity)
{
	const char *s;

	switch (severity) {
		case GL_DEBUG_SEVERITY_HIGH:
			s="high";
			break;
		case GL_DEBUG_SEVERITY_MEDIUM:
			s="medium";
			break;
		case GL_DEBUG_SEVERITY_LOW:
			s="low";
			break;
		case GL_DEBUG_SEVERITY_NOTIFICATION:
			s="notification";
			break;
		default:
			s="[UNKNOWN SEVERITY]";
	}

	return s;
}

/* our debug callback */
static void APIENTRY
GH_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
		  GLsizei length, const GLchar *message, const GLvoid* userParam)
{
	const gl_context_t *glc=(const gl_context_t*)userParam;
	if (glc) {
		GH_verbose(GH_MSG_INFO, "GLDEBUG: %s %s %s [0x%x]: %s\n",
					translateDebugSourceEnum(source),
					translateDebugTypeEnum(type),
					translateDebugSeverityEnum(severity),
					id, message);
		if (glc->original_debug_callback) {
			glc->original_debug_callback(source, type, id, severity,
						     length, message, glc->original_debug_callback_user_ptr);
		}
	}
}

/* our debug callback AMD */
static void APIENTRY
GH_debug_callback_AMD(GLuint id, GLenum category, GLenum severity,
		  GLsizei length, const GLchar *message, GLvoid* userParam)
{
	const gl_context_t *glc=(const gl_context_t*)userParam;

	if (glc) {
		GH_verbose(GH_MSG_INFO, "GLDEBUG[AMD]: %s %s %s [0x%x]: %s\n",
					translateDebugCategoryEnum(category),
					translateDebugSeverityEnum(severity),
					id, message);
		if (glc->original_debug_callback_AMD) {
			glc->original_debug_callback_AMD(id, category, severity,
							 length, message,
							 glc->original_debug_callback_AMD_user_ptr);
		}
	}
}

/***************************************************************************
 * GL context creation overrides                                           *
 ***************************************************************************/

static void context_creation_opts_init(gl_context_creation_opts_t *opts)
{
	opts->force_profile_on = 0;
	opts->force_profile_off = 0;
	opts->force_flags_on = 0;
	opts->force_flags_off = 0;
	opts->force_no_error = -1;

	if (get_envi("GH_FORCE_GL_CONTEXT_PROFILE_CORE", 0)) {
		opts->force_profile_on = GLX_CONTEXT_CORE_PROFILE_BIT_ARB;
		opts->force_profile_off = GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
	}

	int value = get_envi("GH_FORCE_GL_CONTEXT_PROFILE_COMPAT", 0);
	if (value == 1) {
		opts->force_profile_on = GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
		opts->force_profile_off = GLX_CONTEXT_CORE_PROFILE_BIT_ARB;
	} else if (value == 2) {
		opts->flags |= GH_GLCTX_COMPAT_IF_LEGACY;
	}
	if (get_envi("GH_FORCE_GL_CONTEXT_FLAGS_NO_FORWARD_COMPAT", 0)) {
		opts->force_flags_on &= ~GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB;
		opts->force_flags_off |= GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB;
	}
	if (get_envi("GH_FORCE_GL_CONTEXT_FLAGS_NO_DEBUG", 0)) {
		opts->force_flags_on &= ~GLX_CONTEXT_DEBUG_BIT_ARB;
		opts->force_flags_off |= GLX_CONTEXT_DEBUG_BIT_ARB;
	}
	if (get_envi("GH_FORCE_GL_CONTEXT_FLAGS_FORWARD_COMPAT", 0)) {
		opts->force_flags_on |= GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB;
		opts->force_flags_off &= ~GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB;
	}
	if (get_envi("GH_FORCE_GL_CONTEXT_FLAGS_DEBUG", 0)) {
		opts->force_flags_on |= GLX_CONTEXT_DEBUG_BIT_ARB;
		opts->force_flags_off &= ~GLX_CONTEXT_DEBUG_BIT_ARB;
	}
	if (get_envi("GH_FORCE_GL_CONTEXT_FLAGS_NO_ERROR", 0)) {
		opts->force_no_error = GL_TRUE;
	}
	if (get_envi("GH_FORCE_GL_CONTEXT_FLAGS_ERROR", 0)) {
		opts->force_no_error = GL_FALSE;
	}

	opts->force_version[0] = get_envi("GH_FORCE_GL_VERSION_MAJOR", opts->force_version[0]);
	opts->force_version[1] = get_envi("GH_FORCE_GL_VERSION_MINOR", opts->force_version[1]);

	opts->force_version_min[0] = get_envi("GH_FORCE_MIN_GL_VERSION_MAJOR", opts->force_version_min[0]);
	opts->force_version_min[1] = get_envi("GH_FORCE_MIN_GL_VERSION_MINOR", opts->force_version_min[1]);

	opts->force_version_max[0] = get_envi("GH_FORCE_MAX_GL_VERSION_MAJOR", opts->force_version_max[0]);
	opts->force_version_max[1] = get_envi("GH_FORCE_MAX_GL_VERSION_MINOR", opts->force_version_max[1]);
	
	opts->force_flags_on = get_envi("GH_FORCE_GL_CONTEXT_FLAGS_ON", opts->force_flags_on);
	opts->force_flags_off = get_envi("GH_FORCE_GL_CONTEXT_FLAGS_OFF", opts->force_flags_off);

	opts->force_profile_on = get_envi("GH_FORCE_GL_CONTEXT_PROFILE_MASK_ON", opts->force_profile_on);
	opts->force_profile_off = get_envi("GH_FORCE_GL_CONTEXT_PROFILE_MASK_OFF", opts->force_profile_off);

	GH_verbose(GH_MSG_DEBUG, "got GL override options: force version %d.%d, min %d.%d, max %d.%d, flags +0x%x -0x%x, profile flags: +0x%x -0x%x, no error: %d\n",
		opts->force_version[0],opts->force_version[1],
		opts->force_version_min[0], opts->force_version_min[1],
		opts->force_version_max[0], opts->force_version_max[1],
		(unsigned)opts->force_flags_on, (unsigned)opts->force_flags_off,
		(unsigned)opts->force_profile_on, (unsigned)opts->force_profile_off,
		opts->force_no_error);
}

static int need_creation_override(const gl_context_creation_opts_t *opts)
{
	if (opts->force_version[0] >= 0 || opts->force_version[1] >= 0) {
		return 1;
	}
	if (opts->force_version_min[0] >= 0 || opts->force_version_min[1] >= 0) {
		return 1;
	}
	if (opts->force_version_max[0] >= 0 || opts->force_version_max[1] >= 0) {
		return 1;
	}
	if (opts->force_flags_on || opts->force_flags_off) {
		return 2;
	}
	if (opts->force_profile_on || opts->force_profile_off) {
		return 3;
	}
	if (opts->force_no_error >= 0) {
		return 4;
	}

	return 0;
}

static int* get_override_attributes(gl_context_creation_opts_t *opts, const int *attribs)
{
	const int our_count=5;
	int count = 0;
	int additional_count = 0;
	int req_version[2] = {1,0};
	int req_profile_mask = GLX_CONTEXT_CORE_PROFILE_BIT_ARB;
	int req_no_error = -1;
	int req_flags = 0;
	int legacy = 0;
	int *attr_override;
	int pos=0;
	int i;

	GH_verbose(GH_MSG_INFO, "overriding context attributes for creation\n");
	if (attribs) {
		while (attribs[2*count] != None) {
			unsigned int name = (unsigned)attribs[2*count];
			int value = attribs[2*count + 1];
			GH_verbose(GH_MSG_INFO, "originally requested attrib: 0x%x = %d\n", name, value);
			switch(name) {
				case GLX_CONTEXT_MAJOR_VERSION_ARB:
					req_version[0] = value;
					break;
				case GLX_CONTEXT_MINOR_VERSION_ARB:
					req_version[1] = value;
					break;
				case GLX_CONTEXT_PROFILE_MASK_ARB:
					req_profile_mask = value;
					break;
				case GLX_CONTEXT_FLAGS_ARB:
					req_flags = value;
					break;
				case GLX_CONTEXT_OPENGL_NO_ERROR_ARB:
					req_no_error = value;
					break;
				default:
					additional_count++;
			}
			count++;
		}
	}

	legacy = ((req_version[0] < 3) || ( (req_version[0] == 3) && req_version[1] < 2) );
	if (legacy && (opts->flags & GH_GLCTX_COMPAT_IF_LEGACY)) {
		GH_verbose(GH_MSG_INFO, "overriding legacy context to compat profile\n");
		req_profile_mask &= ~GLX_CONTEXT_CORE_PROFILE_BIT_ARB;
		req_profile_mask |= GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
	}

	if (opts->force_version_min[0] >= 0) {
		if (req_version[0] < opts->force_version_min[0]) {
			GH_verbose(GH_MSG_INFO, "overriding context major version from %d to %d [min]\n",
				   req_version[0], opts->force_version_min[0]);
			req_version[0] = opts->force_version_min[0];
		}
	}
	if (opts->force_version_min[1] >= 0) {
		int override=0;
		if (opts->force_version_min[0] >= 0) {
			if (req_version[0] <= opts->force_version_min[0]) {
				if (req_version[1] < opts->force_version_min[1]) {
					override = 1;
				}
			}
		} else {
			if (req_version[1] < opts->force_version_min[1]) {
				override = 1;
			}
		}

		if (override) {
			GH_verbose(GH_MSG_INFO, "overriding context minor version from %d to %d [min]\n",
				   req_version[1], opts->force_version_min[1]);
			req_version[1] = opts->force_version_min[1];
		}
	}

	if (opts->force_version_max[0] >= 0) {
		if (req_version[0] > opts->force_version_max[0]) {
			GH_verbose(GH_MSG_INFO, "overriding context major version from %d to %d [max]\n",
				   req_version[0], opts->force_version_max[0]);
			req_version[0] = opts->force_version_max[0];
		}
	}
	if (opts->force_version_max[1] >= 0) {
		int override=0;
		if (opts->force_version_max[0] >= 0) {
			if (req_version[0] >= opts->force_version_max[0]) {
				if (req_version[1] > opts->force_version_max[1]) {
					override = 1;
				}
			}
		} else {
			if (req_version[1] > opts->force_version_max[1]) {
				override = 1;
			}
		}

		if (override) {
			GH_verbose(GH_MSG_INFO, "overriding context minor version from %d to %d [max]\n",
				   req_version[1], opts->force_version_max[1]);
			req_version[1] = opts->force_version_max[1];
		}
	}

	if (opts->force_version[0] >= 0) {
		GH_verbose(GH_MSG_INFO, "overriding context major version from %d to %d\n",
			   req_version[0], opts->force_version[0]);
		req_version[0] = opts->force_version[0];
	}
	if (opts->force_version[1] >= 0) {
		GH_verbose(GH_MSG_INFO, "overriding context minor version from %d to %d\n",
			   req_version[1], opts->force_version[1]);
		req_version[1] = opts->force_version[1];
	}
	if (opts->force_flags_on || opts->force_flags_off) {
		int new_flags = (req_flags | opts->force_flags_on)&(~opts->force_flags_off);
		GH_verbose(GH_MSG_INFO, "overriding context flags from 0x%x to 0x%x\n",
			   (unsigned)req_flags, (unsigned)new_flags);
		req_flags = new_flags;
	}
	if (opts->force_profile_on || opts->force_profile_off) {
		int new_profile = (req_profile_mask | opts->force_profile_on)&(~opts->force_profile_off);
		GH_verbose(GH_MSG_INFO, "overriding context profile mask from 0x%x to 0x%x\n",
			   (unsigned)req_profile_mask, (unsigned)new_profile);
		req_profile_mask = new_profile;
	}
	if (opts->force_no_error >= 0) {
		GH_verbose(GH_MSG_INFO, "overriding context NO_ERROR behavior from %d to %d\n",
			   req_no_error, opts->force_no_error);
		req_no_error = opts->force_no_error;
	}

	attr_override = malloc(sizeof(*attr_override) * ((additional_count + our_count) * 2 + 2));
	if (!attr_override) {
		GH_verbose(GH_MSG_ERROR, "overriding context attributes not possible: out of memory\n");
		return NULL;
	}

	GH_verbose(GH_MSG_INFO, "requesting GL %d.%d flags: 0x%x, profile: 0x%x\n",
		   req_version[0], req_version[1], (unsigned)req_flags, (unsigned)req_profile_mask);
	attr_override[pos++] = GLX_CONTEXT_MAJOR_VERSION_ARB;
	attr_override[pos++] = req_version[0];
	attr_override[pos++] = GLX_CONTEXT_MINOR_VERSION_ARB;
	attr_override[pos++] = req_version[1];
	attr_override[pos++] = GLX_CONTEXT_PROFILE_MASK_ARB;
	attr_override[pos++] = req_profile_mask;
	attr_override[pos++] = GLX_CONTEXT_FLAGS_ARB;
	attr_override[pos++] = req_flags;
	if (req_no_error >= 0) {
		attr_override[pos++] = GLX_CONTEXT_OPENGL_NO_ERROR_ARB;
		attr_override[pos++] = req_no_error;
	}

	for (i=0; i<count; i++) {
		unsigned int name = (unsigned)attribs[2*i];
		int value = attribs[2*i+1];
		switch (name) {
			case GLX_CONTEXT_MAJOR_VERSION_ARB:
			case GLX_CONTEXT_MINOR_VERSION_ARB:
			case GLX_CONTEXT_PROFILE_MASK_ARB:
			case GLX_CONTEXT_FLAGS_ARB:
			case GLX_CONTEXT_OPENGL_NO_ERROR_ARB:
				(void)0;
				break;
			default:
				attr_override[pos++] = (int)name;
				attr_override[pos++] = value;
		}
	}
	attr_override[pos++] = None;
	attr_override[pos++] = None;

	return attr_override;
}

static const GLXFBConfig* get_fbconfig_for_visual(Display *dpy, XVisualInfo *vis, GLXFBConfig *cfg)
{
	GLXFBConfig *cfgs;
	const GLXFBConfig *new_cfg = NULL;
	/* TODO: what about the screen number? */
	int screen=0;
	int count;

	GH_GET_PTR_GL(glXGetFBConfigs);
	GH_GET_PTR_GL(glXGetFBConfigAttrib);
	GH_GET_PTR(XFree);

	if (!GH_glXGetFBConfigs || !GH_glXGetFBConfigAttrib || !GH_XFree) {
		GH_verbose(GH_MSG_ERROR, "glXGetFBConfigs or glXGetFBConfigAttrib or XFree not found!\n");
		return NULL;
	}

	cfgs=GH_glXGetFBConfigs(dpy, screen, &count);

	if (cfgs && count > 0) {
		int i;
		for (i=0; i<count; i++) {
			int value = -1;
			int res = GH_glXGetFBConfigAttrib(dpy, cfgs[i], GLX_VISUAL_ID, &value);
			if (res == Success) {
				GH_verbose(GH_MSG_DEBUG, "fbconfig %d for visual ID %d\n", i, value);
				if (value == (int)vis->visualid) {
					GH_verbose(GH_MSG_INFO, "found fbconfig %d for visual ID %d\n", i, value);
					*cfg = cfgs[i];
					new_cfg=cfg;
					break;
				}
			} else {
				GH_verbose(GH_MSG_WARNING, "glxGerFBConfigAttrib failed!\n");
			}
		}
	}

	if (cfgs) {
		GH_XFree(cfgs);
	}

	return new_cfg;
}

static GLXContext override_create_context(Display *dpy, XVisualInfo *vis, const GLXFBConfig *fbconfig, GLXContext shareList, Bool direct, const int *attribs)
{
	GLXFBConfig internal_fbconfig;

	pthread_mutex_lock(&ctx_creation_opts.mutex);
	if (!(ctx_creation_opts.flags & GH_GLCTX_CREATE_INITIALIZED)) {
		context_creation_opts_init(&ctx_creation_opts);
		ctx_creation_opts.flags |= GH_GLCTX_CREATE_INITIALIZED;
	}
	pthread_mutex_unlock(&ctx_creation_opts.mutex);

	if (need_creation_override(&ctx_creation_opts)) {
		GLXContext ctx = NULL;
		int *attribs_override = get_override_attributes(&ctx_creation_opts, attribs);
		if (!attribs_override) {
			GH_verbose(GH_MSG_WARNING, "failed to generate context creation override attributes!\n");
			return NULL;
		}
		if (!fbconfig) {
			if (!vis) {
				GH_verbose(GH_MSG_WARNING, "create context attempt without Visual and FBConfig!\n");
				free(attribs_override);
				return NULL;	
			}
			/* find FBConfig vor Visual ... */
			fbconfig=get_fbconfig_for_visual(dpy, vis, &internal_fbconfig);
			if (!fbconfig) {
				GH_verbose(GH_MSG_WARNING, "create context: failed to get fbconfig for visual!\n");
				free(attribs_override);
				return NULL;	
			}
		}
		GH_GET_GL_PROC(glXCreateContextAttribsARB);
		if (GH_glXCreateContextAttribsARB) {
			ctx=GH_glXCreateContextAttribsARB(dpy, *fbconfig, shareList, direct, attribs_override);
		} else {
			GH_verbose(GH_MSG_WARNING, "failed to get glXCreateContextAttribsARB\n");
		}
		if (ctx == NULL) {
			GH_verbose(GH_MSG_WARNING, "overridden context creation failed!\n");
		} else {
			GH_verbose(GH_MSG_INFO, "created context %p with overriden attributes!\n", ctx);
		}
		free(attribs_override);
		return ctx;

	}
	return NULL;
}

#endif /* GH_CONTEXT_TRACKING */

/***************************************************************************
 * SWAP INTERVAL LOGIC                                                     *
 ***************************************************************************/

/* possible GH_SWAP_MODEs */
typedef enum {
	GH_SWAP_MODE_NOP=0,	/* do not change anything */
	GH_SWAP_MODE_IGNORE,	/* ignore all attempts to set swap interval */
	GH_SWAP_MODE_CLAMP,	/* clamp interval to (a,b) */
	GH_SWAP_MODE_FORCE,	/* force interval to a */
	GH_SWAP_MODE_DISABLE,	/* force interval to 0 */
	GH_SWAP_MODE_ENABLE,	/* force interval to >= 1 */
	GH_SWAP_MODE_MIN,	/* force interval to >= a */
	GH_SWAP_MODE_MAX,	/* force interval to <= a */
	/* always add new elements here */
	GH_SWAP_MODES_COUNT
} GH_swap_mode;

/* possible GH_SWAP_TEAR modes */
typedef enum {
	GH_SWAP_TEAR_RAW,	/* handle interval as if normal */
	GH_SWAP_TEAR_KEEP,	/* keep tearing control as requested */
	GH_SWAP_TEAR_DISABLE,	/* always disbale adaptive vsync */
	GH_SWAP_TEAR_ENABLE,	/* enable adaptive vsync */
	GH_SWAP_TEAR_INVERT,	/* could one ever need this? */
	/* always add new elements here */
	GH_SWAP_TEAR_COUNT
} GH_swap_tear;

typedef struct {
	GH_swap_mode swap_mode;
	GH_swap_tear swap_tear;
	int swap_param[2];
} GH_config;

static void GH_swap_mode_from_str(GH_config volatile *cfg, const char *str)
{
	static const char *mode_str[GH_SWAP_MODES_COUNT+1]={
		"nop",
		"ignore",
		"clamp",
		"force",
		"disable",
		"enable",
		"min",
		"max",
		NULL
	};

	size_t l=0;
	int idx;
	char *nstr;

	cfg->swap_mode=GH_SWAP_MODE_NOP;
	
	if (str == NULL)
		return;

	for (idx=0; mode_str[idx]; idx++) {
		l=strlen(mode_str[idx]);
		if (!strncmp(str, mode_str[idx], l)) {
			break;
		}
	}

	if (idx >= 0 && idx<(int)GH_SWAP_MODES_COUNT) {
		cfg->swap_mode=(GH_swap_mode)idx;
	}

	str += l;

	/* read up to 2 ints as arguments */
	while(*str && !isdigit(*str))
		str++;
	cfg->swap_param[0]=(int)strtol(str, &nstr, 0);
	str=nstr;
	while(*str && !isdigit(*str))
		str++;
	cfg->swap_param[1]=(int)strtol(str, &nstr,0);
	GH_verbose(GH_MSG_DEBUG, "SWAP_MODE: %d %d %d\n",
				cfg->swap_mode,cfg->swap_param[0],cfg->swap_param[1]);
}

static void GH_swap_tear_from_str(GH_config volatile *cfg, const char *str)
{
	static const char *mode_str[GH_SWAP_TEAR_COUNT+1]={
		"raw",
		"keep",
		"disable",
		"enable",
		"invert",

		NULL
	};

	int idx;

	cfg->swap_tear=GH_SWAP_TEAR_KEEP;
	
	if (str == NULL)
		return;

	for (idx=0; mode_str[idx]; idx++) {
		if (!strcmp(str, mode_str[idx])) {
			break;
		}
	}

	if (idx >= 0 && idx<(int)GH_SWAP_MODES_COUNT) {
		cfg->swap_tear=(GH_swap_mode)idx;
	}
}

static int GH_swap_interval_absolute(const volatile GH_config *cfg, int interval)
{
	int new_interval;

	switch(cfg->swap_mode) {
		case GH_SWAP_MODE_IGNORE:
			new_interval=GH_SWAP_DONT_SET;
			break;
		case GH_SWAP_MODE_CLAMP:
			new_interval=interval;
			if (new_interval < cfg->swap_param[0])
				new_interval=cfg->swap_param[0];
			if (new_interval > cfg->swap_param[1])
				interval=cfg->swap_param[1];
			break;
		case GH_SWAP_MODE_FORCE:
			new_interval=cfg->swap_param[0];
			break;
		case GH_SWAP_MODE_DISABLE:
			new_interval=0;
			break;
		case GH_SWAP_MODE_ENABLE:
			new_interval=interval;
			if (new_interval < 1)
				new_interval=1;
			break;
		case GH_SWAP_MODE_MIN:
			new_interval=interval;
			if (new_interval < cfg->swap_param[0])
				new_interval=cfg->swap_param[0];
			break;
		case GH_SWAP_MODE_MAX:
			new_interval=interval;
			if (new_interval > cfg->swap_param[0])
				new_interval=cfg->swap_param[0];
			break;
		default:
			new_interval=interval;
	}

	GH_verbose(GH_MSG_DEBUG,"swap interval, absolute value %d -> %d\n", interval, new_interval);
	return new_interval;
}

static int GH_swap_interval_base(const volatile GH_config *cfg, int interval)
{
	int sign_interval;
	int abs_interval;
	int new_interval;

	if (cfg->swap_tear == GH_SWAP_TEAR_RAW) {
		sign_interval=0;
		abs_interval=interval; /* moght be negative */
	} else 	if (interval < 0) {
		sign_interval=-1;
		abs_interval=-interval;
	} else {
		sign_interval=1;
		abs_interval=interval;
	}

	abs_interval=GH_swap_interval_absolute(cfg, abs_interval);
	if (abs_interval == GH_SWAP_DONT_SET) {
		GH_verbose(GH_MSG_INFO,"swap interval %d setting ignored\n",
					interval);
		return GH_SWAP_DONT_SET;
	}

	/* set sign based on GH_SWAP_TEAR mode */
	switch (cfg->swap_tear) {
		case GH_SWAP_TEAR_KEEP:
			new_interval=abs_interval * sign_interval;
			break;
		case GH_SWAP_TEAR_DISABLE:
			new_interval=abs_interval;
			break;
		case GH_SWAP_TEAR_ENABLE:
			new_interval=-abs_interval;
			break;
		case GH_SWAP_TEAR_INVERT:
			new_interval=abs_interval * (-sign_interval);
			break;
		default:
			new_interval=abs_interval;	
	}

	GH_verbose(GH_MSG_INFO,"swap interval %d -> %d\n", interval, new_interval);
	return new_interval;
}

static int GH_swap_interval(int interval)
{
	static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
	static volatile GH_config cfg={GH_SWAP_MODES_COUNT, GH_SWAP_TEAR_KEEP, {0,1}};

	pthread_mutex_lock(&mutex);
	if (cfg.swap_mode >= GH_SWAP_MODES_COUNT) {
		GH_swap_mode_from_str(&cfg, getenv("GH_SWAP_MODE"));
		GH_swap_tear_from_str(&cfg, getenv("GH_SWAP_TEAR"));
	}
	pthread_mutex_unlock(&mutex);
	return GH_swap_interval_base(&cfg, interval);
}


/***************************************************************************
 * INTERCEPTED FUNCTIONS: libdl/libc                                       *
 ***************************************************************************/

/* intercept dlsym() itself */
extern void *
dlsym(void *handle, const char *name)
{
	void *interceptor;
	void *ptr;
	/* special case: we cannot use GH_GET_PTR as it relies on
	 * GH_dlsym() which we have to query using GH_dlsym_internal */
	pthread_mutex_lock(&GH_fptr_mutex);
	GH_dlsym_internal_dlsym();
	pthread_mutex_unlock(&GH_fptr_mutex);
	interceptor=GH_get_interceptor(name, GH_dlsym_next, "dlsym");
	ptr=(interceptor)?interceptor:GH_dlsym(handle,name);
	GH_verbose(GH_MSG_DEBUG_INTERCEPTION,"dlsym(%p, %s) = %p%s\n",handle,name,ptr,
		interceptor?" [intercepted]":"");
	return ptr;
}

#if (GH_DLSYM_METHOD != 2)
/* Also intercept GNU specific dlvsym().
 * We can do this only if we are not using dlvsym to query dlsym... */
extern void *
dlvsym(void *handle, const char *name, const char *version)
{
	void *interceptor;
	void *ptr;
	pthread_mutex_lock(&GH_fptr_mutex);
	GH_dlsym_internal_dlsym();
	pthread_mutex_unlock(&GH_fptr_mutex);
	GH_GET_PTR(dlvsym);
	interceptor=GH_get_interceptor(name, GH_dlsym_next, "dlvsym");
	ptr=(interceptor)?interceptor:GH_dlvsym(handle,name,version);
	GH_verbose(GH_MSG_DEBUG_INTERCEPTION,"dlvsym(%p, %s, %s) = %p%s\n",handle,name,version,ptr,
		interceptor?" [intercepted]":"");
	return ptr;
}
#endif

/***************************************************************************
 * INTERCEPTED FUNCTIONS: glX                                              *
 ***************************************************************************/

/* Actually, our goal is to intercept glXSwapInterval[EXT|SGI]() etc. But
 * these are extension functions not required to be provided as external
 * symbols. However, some applications just likn them anyways, so we have
 * to handle the case were dlsym() or glXGetProcAddress[ARB]() is used to
 * query the function pointers, and have to intercept these as well.
 */

/* helper macro to handle both glxGetProcAddress and glXGetProcAddressARB */
#define GH_GLXGETPROCADDRESS_GENERIC(procname) \
extern GH_fptr procname(const GLubyte *name) \
{ \
	void *interceptor; \
	GH_fptr ptr; \
	GH_GET_PTR(procname); \
	interceptor=GH_get_interceptor((const char *)name, \
					(GH_resolve_func)GH_ ##procname, \
					 #procname); \
	ptr=(interceptor)?(GH_fptr)interceptor:GH_ ##procname((const char*)name); \
	GH_verbose(GH_MSG_DEBUG_INTERCEPTION,#procname "(%s) = %p%s\n",(const char *)name, ptr, \
		interceptor?" [intercepted]":""); \
	return ptr; \
}

GH_GLXGETPROCADDRESS_GENERIC(glXGetProcAddress)
GH_GLXGETPROCADDRESS_GENERIC(glXGetProcAddressARB)

#ifdef GH_CONTEXT_TRACKING

/* ---------- Context Creation ---------- */

extern GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct )
{
	GLXContext ctx;

	ctx = override_create_context(dpy, vis, NULL, shareList, direct, NULL);
	if (ctx == NULL) {
		GH_GET_PTR_GL(glXCreateContext);
		ctx=GH_glXCreateContext(dpy, vis, shareList, direct);
	}
	create_context(ctx);
	return ctx;
}

extern GLXContext glXCreateNewContext( Display *dpy, GLXFBConfig config, int renderType, GLXContext shareList, Bool direct )
{
	GLXContext ctx;

	ctx = override_create_context(dpy, NULL, &config, shareList, direct, NULL);
	if (ctx == NULL) {
		GH_GET_PTR_GL(glXCreateNewContext);
		ctx=GH_glXCreateNewContext(dpy, config, renderType, shareList, direct);
	}
	create_context(ctx);
	return ctx;
}

extern GLXContext glXCreateContextAttribsARB (Display * dpy, GLXFBConfig config, GLXContext shareList, Bool direct, const int *attr)
{
	GLXContext ctx;

	ctx = override_create_context(dpy, NULL, &config, shareList, direct, attr);
	if (ctx == NULL) {
		GH_GET_PTR_GL(glXCreateContextAttribsARB);
		ctx=GH_glXCreateContextAttribsARB(dpy, config, shareList, direct, attr);
	}
	create_context(ctx);
	return ctx;
}

extern GLXContext glXImportContextEXT (Display *dpy, GLXContextID id)
{
	GLXContext ctx;

	GH_GET_PTR_GL(glXImportContextEXT);
	ctx=GH_glXImportContextEXT(dpy, id);
	create_context(ctx);
	return ctx;
}

extern GLXContext glXCreateContextWithConfigSGIX (Display *dpy, GLXFBConfigSGIX config, int renderType, GLXContext shareList, Bool direct)
{
	GLXContext ctx;

	/* TODO: override_create_context for this case */
	GH_GET_PTR_GL(glXCreateContextWithConfigSGIX);
	ctx=GH_glXCreateContextWithConfigSGIX(dpy, config, renderType, shareList, direct);
	create_context(ctx);
	return ctx;
}

/* ---------- Context Destruction ---------- */

extern void glXDestroyContext(Display *dpy, GLXContext ctx)
{
	GH_GET_PTR_GL(glXDestroyContext);
	GH_glXDestroyContext(dpy, ctx);
	destroy_context(ctx);
}

extern void glXFreeContextEXT(Display *dpy, GLXContext ctx)
{
	GH_GET_PTR_GL(glXFreeContextEXT);
	GH_glXFreeContextEXT(dpy, ctx);
	destroy_context(ctx);
}

/* ---------- Current Context Tracking ---------- */

extern Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
	Bool result;

	result=GH_glXMakeCurrent(dpy, drawable, ctx);
	make_current(ctx, dpy, drawable, drawable);
	return result;
}

extern Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
	Bool result;

	result=GH_glXMakeContextCurrent(dpy, draw, read, ctx);
	make_current(ctx, dpy, draw, read);
	return result;
}

extern Bool glXMakeCurrentReadSGI(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
	Bool result;

	result=GH_glXMakeCurrentReadSGI(dpy, draw, read, ctx);
	make_current(ctx, dpy, draw, read);
	return result;
}

/* ---------- GL Debug Output ---------- */

extern void glDebugMessageCallback(GLDEBUGPROC proc, const GLvoid *user_ptr)
{
	GH_GET_PTR_GL(glDebugMessageCallback);
	if (GH_glDebugMessageCallback) {
		gl_context_t *glc=(gl_context_t*)pthread_getspecific(ctx_current);
		if (glc) {
			glc->original_debug_callback = proc;
			glc->original_debug_callback_user_ptr = user_ptr;

			if (glc->flags & GH_GL_INTERCEPT_DEBUG) {
				GH_verbose(GH_MSG_INFO, "injecting debug output [core]!\n");
				proc = GH_debug_callback;
				user_ptr = glc;
			}
		} else {
			GH_verbose(GH_MSG_WARNING, "debug message callback without current context!\n");
		}
		GH_glDebugMessageCallback(proc, user_ptr);
	}
}

extern void glDebugMessageCallbackARB(GLDEBUGPROC proc, const GLvoid *user_ptr)
{
	GH_GET_PTR_GL(glDebugMessageCallbackARB);
	if (GH_glDebugMessageCallbackARB) {
		gl_context_t *glc=(gl_context_t*)pthread_getspecific(ctx_current);
		if (glc) {
			glc->original_debug_callback = proc;
			glc->original_debug_callback_user_ptr = user_ptr;

			if (glc->flags & GH_GL_INTERCEPT_DEBUG) {
				GH_verbose(GH_MSG_INFO, "injecting debug output [ARB]!\n");
				proc = GH_debug_callback;
				user_ptr = glc;
			}
		} else {
			GH_verbose(GH_MSG_WARNING, "debug message callback without current context!\n");
		}
		GH_glDebugMessageCallbackARB(proc, user_ptr);
	}
}

extern void glDebugMessageCallbackKHR(GLDEBUGPROC proc, const GLvoid *user_ptr)
{
	GH_GET_PTR_GL(glDebugMessageCallbackKHR);
	if (GH_glDebugMessageCallbackKHR) {
		gl_context_t *glc=(gl_context_t*)pthread_getspecific(ctx_current);
		if (glc) {
			glc->original_debug_callback = proc;
			glc->original_debug_callback_user_ptr = user_ptr;

			if (glc->flags & GH_GL_INTERCEPT_DEBUG) {
				GH_verbose(GH_MSG_INFO, "injecting debug output [KHR]!\n");
				proc = GH_debug_callback;
				user_ptr = glc;
			}
		} else {
			GH_verbose(GH_MSG_WARNING, "debug message callback without current context!\n");
		}
		GH_glDebugMessageCallbackKHR(proc, user_ptr);
	}
}

extern void glDebugMessageCallbackAMD(GLDEBUGPROCAMD proc, GLvoid *user_ptr)
{
	GH_GET_PTR_GL(glDebugMessageCallbackAMD);
	if (GH_glDebugMessageCallbackAMD) {
		gl_context_t *glc=(gl_context_t*)pthread_getspecific(ctx_current);
		if (glc) {
			glc->original_debug_callback_AMD = proc;
			glc->original_debug_callback_AMD_user_ptr = user_ptr;

			if (glc->flags & GH_GL_INTERCEPT_DEBUG) {
				GH_verbose(GH_MSG_INFO, "injecting debug output [AMD]!\n");
				proc = GH_debug_callback_AMD;
				user_ptr = glc;
			}
		} else {
			GH_verbose(GH_MSG_WARNING, "debug message callback without current context!\n");
		}
		GH_glDebugMessageCallbackAMD(proc, user_ptr);
	}
}

#endif /* GH_CONTEXT_TRACKING */

/* ---------- Swap Interval---------- */

extern void glXSwapIntervalEXT(Display *dpy, GLXDrawable drawable,
				int interval)
{
	interval=GH_swap_interval(interval);
	if (interval == GH_SWAP_DONT_SET) {
		/* ignore the call */
		return;
	}
	GH_GET_PTR_GL(glXSwapIntervalEXT);
	GH_glXSwapIntervalEXT(dpy, drawable, interval);
}

extern int glXSwapIntervalSGI(int interval)
{
	interval=GH_swap_interval(interval);
	if (interval == GH_SWAP_DONT_SET) {
		/* ignore the call */
		return 0; /* success */
	}
	GH_GET_PTR_GL(glXSwapIntervalSGI);
	return GH_glXSwapIntervalSGI(interval);
}

extern int glXSwapIntervalMESA(unsigned int interval)
{
	int signed_interval;
	if (interval > (unsigned)INT_MAX) {
		signed_interval=INT_MAX;
	} else {
		signed_interval=(int)interval;
	}
	signed_interval=GH_swap_interval(signed_interval);
	if (signed_interval == GH_SWAP_DONT_SET) {
		/* ignore the call */
		return 0; /* success */
	}
	if (signed_interval < 0) {
		GH_verbose(GH_MSG_WARNING,"glXSwapIntervalMESA does not support negative swap intervals\n");
		interval=(unsigned)(-signed_interval);
	} else {
		interval=(unsigned)signed_interval;
	}
	GH_GET_PTR_GL(glXSwapIntervalMESA);
	return GH_glXSwapIntervalMESA(interval);
}

/* ---------- Swap Buffers ---------- */

#ifdef GH_SWAPBUFFERS_INTERCEPT
extern void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
#ifdef GH_CONTEXT_TRACKING
	gl_context_t *glc=(gl_context_t*)pthread_getspecific(ctx_current);

	if (glc) {
		frametimes_before_swap(&glc->frametimes);
		if (glc->swapbuffer_omission.swapbuffers > 0) {
			int do_swap;
			if (glc->swapbuffer_omission.latency_mode > 0) {
				latency_before_swap(&glc->latency);
			}
			do_swap = swapbuffer_omission_do_swap(&glc->swapbuffer_omission);
			if (do_swap) {
				if (glc->swapbuffer_omission.latency_mode < 1) {
					latency_before_swap(&glc->latency);
					GH_glXSwapBuffers(dpy, drawable);
					latency_after_swap(&glc->latency);
				} else {
					GH_glXSwapBuffers(dpy, drawable);
				}
			} else {
				swapbuffer_omission_swap_skipped(&glc->swapbuffer_omission);
			}
			if (glc->swapbuffer_omission.latency_mode > 0) {
				latency_after_swap(&glc->latency);
			}
			swapbuffer_omission_swap_finished(&glc->swapbuffer_omission, do_swap);
		} else {
			latency_before_swap(&glc->latency);
			GH_glXSwapBuffers(dpy, drawable);
			latency_after_swap(&glc->latency);
		}
		frametimes_after_swap(&glc->frametimes);
		if (glc->swap_sleep_usecs) {
			usleep(glc->swap_sleep_usecs);
		}
	} else {
		GH_verbose(GH_MSG_WARNING,"SwapBuffers called without a context\n");
		GH_GET_PTR_GL(glXSwapBuffers);
		GH_glXSwapBuffers(dpy, drawable);
	}
#else /* GH_CONTEXT_TRACKING */
	GH_GET_PTR_GL(glXSwapBuffers);
	GH_glXSwapBuffers(dpy, drawable);
#endif /* GH_CONTEXT_TRACKING */
}
#endif /* GH_SWAPBUFFERS_INTERCEPT */

/* ---------- Tex Parameter ---------- */


/* EVIL HACK, do not use this version!!!! */

static GLint base_level = 1;

extern void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	GLfloat setAniso = -1.0f;
	GH_GET_PTR_GL(glTexParameteri);
	
	if (pname == GL_TEXTURE_MIN_FILTER) {
		if (param == GL_NEAREST_MIPMAP_NEAREST || param == GL_NEAREST_MIPMAP_LINEAR || param == GL_LINEAR_MIPMAP_NEAREST) {
			param = GL_LINEAR_MIPMAP_LINEAR;
			setAniso = 16.0f;
		}
	}
	GH_glTexParameteri(target, pname, param);
	if (setAniso >= 0.0f) {
		GH_GET_PTR_GL(glTexParameterf);
		GH_glTexParameterf(target, GL_MAX_ANISOTROPY_EXT, setAniso);
	}
}

extern void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
	GH_GET_PTR_GL(glTexParameterf);
	if (pname == GL_TEXTURE_MIN_FILTER) {
		GLint p = (GLint)(param);
		if (p == GL_NEAREST_MIPMAP_NEAREST || p == GL_NEAREST_MIPMAP_LINEAR || p == GL_LINEAR_MIPMAP_NEAREST) {
			p = GL_LINEAR_MIPMAP_LINEAR;
			GH_glTexParameteri(target, pname, p);
			GH_glTexParameterf(target, GL_MAX_ANISOTROPY_EXT, 16.0f);
			return;
		}

	}
	GH_glTexParameterf(target, pname, param);
}

/* EVEN MORE EVIL HACK, doesn't even work as intended...
extern void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
{
	GH_GET_PTR_GL(glTexStorage2D);
	if (width == 256 && height == 256) {
		int maxLevelW = (int)log2(width);
		int maxLevelH = (int)log2(height);
		if (maxLevelW < maxLevelH) { maxLevelW = maxLevelH;}
		if (levels < base_level+1) {
			levels = base_level + 1;
			if (levels > maxLevelW) {
				levels = maxLevelW;
			}
		}
	}
	GH_glTexStorage2D(target, levels, internalformat, width, height);
}

extern void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid * pixels)
{
	GH_GET_PTR_GL(glTexSubImage2D);
	GH_GET_PTR_GL(glGenerateMipmap);
	GH_GET_PTR_GL(glTexParameteri);
	GH_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);

	if (width == 256 && height == 256 && level == 0) {
		GH_glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
		GH_glGenerateMipmap(target);
		GH_glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, base_level);
	}
}

extern void glTexImage2D(GLenum target, GLint level,  GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid * data)
{
	GH_GET_PTR_GL(glTexImage2D);
	GH_GET_PTR_GL(glGenerateMipmap);
	GH_GET_PTR_GL(glTexParameteri);
	GH_glTexImage2D(target, level, internalFormat, width, height, border, format, type, data);

	if (width == 256 && height == 256 && level == 0) {
		GH_glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
		GH_glGenerateMipmap(target);
		GH_glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, base_level);
	}
}

*/

/***************************************************************************
 * LIST OF INTERCEPTED FUNCTIONS                                           *
 ***************************************************************************/

/* return intercepted fuction pointer for "name", or NULL if
 * "name" is not to be intercepted. If function is intercepted,
 * use query to resolve the original function pointer and store
 * it in the GH_"name" static pointer. That way, we use the same
 * function the original application were using without the interceptor.
 * The interceptor functions will fall back to using GH_dlsym() if the
 * name resolution here did fail for some reason.
 */
static void* GH_get_interceptor(const char *name, GH_resolve_func query,
				const char *query_name )
{
#define GH_INTERCEPT(func) \
       	if (!strcmp(#func, name)) {\
		pthread_mutex_lock(&GH_fptr_mutex); \
		if ( (GH_ ##func == NULL) && query) { \
			GH_ ##func = query(#func); \
			GH_verbose(GH_MSG_DEBUG,"queried internal %s via %s: %p\n", \
				name,query_name, GH_ ##func); \
		} \
		pthread_mutex_unlock(&GH_fptr_mutex); \
		return func; \
	}

#ifdef GH_SWAPBUFFERS_INTERCEPT
	static int do_swapbuffers=0;
#endif
	static int do_dlsym = 0;
#if (GH_DLSYM_METHOD != 2)
	static int do_dlvsym = 0;
#endif
	static int inited = 0;

	if (!inited) {
#ifdef GH_SWAPBUFFERS_INTERCEPT
		do_swapbuffers =get_envi("GH_SWAPBUFFERS", 0) ||
				get_envi("GH_FRAMETIME", 0) ||
				get_envi("GH_SWAP_SLEEP_USECS", 0) ||
				(get_envi("GH_LATENCY", GH_LATENCY_NOP) != GH_LATENCY_NOP);
#endif
		do_dlsym = get_envi("GH_HOOK_DLSYM_DYNAMICALLY", 0);
#if (GH_DLSYM_METHOD != 2)
		do_dlvsym = get_envi("GH_HOOK_DLVSYM_DYNAMICALLY", 0);
#endif
		inited = 1;
	}

	if (do_dlsym) {
		GH_INTERCEPT(dlsym);
	}
#if (GH_DLSYM_METHOD != 2)
	if (do_dlvsym) {
		GH_INTERCEPT(dlvsym);
	}
#endif
	GH_INTERCEPT(glXGetProcAddress);
	GH_INTERCEPT(glXGetProcAddressARB);
	GH_INTERCEPT(glXSwapIntervalEXT);
	GH_INTERCEPT(glXSwapIntervalSGI);
	GH_INTERCEPT(glXSwapIntervalMESA);
#ifdef GH_CONTEXT_TRACKING
	GH_INTERCEPT(glXCreateContext);
	GH_INTERCEPT(glXCreateNewContext);
	GH_INTERCEPT(glXCreateContextAttribsARB);
	GH_INTERCEPT(glXImportContextEXT);
	GH_INTERCEPT(glXCreateContextWithConfigSGIX);
	GH_INTERCEPT(glXDestroyContext);
	GH_INTERCEPT(glXFreeContextEXT);
	GH_INTERCEPT(glXMakeCurrent);
	GH_INTERCEPT(glXMakeContextCurrent);
	GH_INTERCEPT(glXMakeCurrentReadSGI);
	GH_INTERCEPT(glDebugMessageCallback);
	GH_INTERCEPT(glDebugMessageCallbackARB);
	GH_INTERCEPT(glDebugMessageCallbackKHR);
	GH_INTERCEPT(glDebugMessageCallbackAMD);
	GH_INTERCEPT(glTexParameteri);
	GH_INTERCEPT(glTexParameterf);
	/*
	GH_INTERCEPT(glTexStorage2D);
	GH_INTERCEPT(glTexSubImage2D);
	GH_INTERCEPT(glTexImage2D);
	*/
#endif
#ifdef GH_SWAPBUFFERS_INTERCEPT
	if (do_swapbuffers) {
		if (do_swapbuffers < 0) {
			do_swapbuffers =get_envi("GH_SWAPBUFFERS", 0) ||
					get_envi("GH_FRAMETIME", 0) ||
					get_envi("GH_SWAP_SLEEP_USECS", 0) ||
					(get_envi("GH_LATENCY", GH_LATENCY_NOP) != GH_LATENCY_NOP);
		}
		if (do_swapbuffers)
			GH_INTERCEPT(glXSwapBuffers);
	}
#endif
	return NULL;
}

