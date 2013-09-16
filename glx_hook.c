#define _GNU_SOURCE
#include <dlfcn.h>	/* for RTLD_NEXT */
#include <pthread.h>	/* for mutextes */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

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
		const char *gh_verbose=getenv("GH_VERBOSE");
		if (gh_verbose)
			verbosity=(int)strtol(gh_verbose,NULL,0);
		if (verbosity < 0)
			verbosity=GH_MSG_LEVEL_DEFAULT;
	}

	if (level > verbosity) {
		return;
	}

	if (!stream_initialized) {
		const char *file=getenv("GH_VERBOSE_FILE");
		if (file) 
			output_stream=fopen(file,"a+t");
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
 * SWAP INTERVAL LOGIC                                                     *
 ***************************************************************************/

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
} GH_mode;

typedef struct {
	GH_mode mode;
	int value[2];
} GH_config;

static void GH_config_from_str(GH_config volatile *cfg, const char *str)
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

	cfg->mode=GH_SWAP_MODE_NOP;
	
	if (str == NULL)
		return;

	for (idx=0; mode_str[idx]; idx++) {
		l=strlen(mode_str[idx]);
		if (!strncmp(str, mode_str[idx], l)) {
			break;
		}
	}

	if (idx >= 0 && idx<(int)GH_SWAP_MODES_COUNT) {
		cfg->mode=(GH_mode)idx;
	}

	str += l;

	/* read up to 2 ints as arguments */
	while(*str && !isdigit(*str))
		str++;
	cfg->value[0]=(int)strtol(str, &nstr, 0);
	str=nstr;
	while(*str && !isdigit(*str))
		str++;
	cfg->value[1]=(int)strtol(str, &nstr,0);
	GH_verbose(GH_MSG_DEBUG, "CONFIG: %d %d %d\n",cfg->mode,cfg->value[0],cfg->value[1]);
}

static int GH_swap_interval_for_cfg(const volatile GH_config *cfg, int interval)
{
	int new_interval;

	switch(cfg->mode) {
		case GH_SWAP_MODE_IGNORE:
			new_interval=-1;
			break;
		case GH_SWAP_MODE_CLAMP:
			new_interval=interval;
			if (new_interval < cfg->value[0])
				new_interval=cfg->value[0];
			if (new_interval > cfg->value[1])
				interval=cfg->value[1];
			break;
		case GH_SWAP_MODE_FORCE:
			new_interval=cfg->value[0];
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
			if (new_interval < cfg->value[0])
				new_interval=cfg->value[0];
			break;
		case GH_SWAP_MODE_MAX:
			new_interval=interval;
			if (new_interval > cfg->value[0])
				new_interval=cfg->value[0];
			break;
		default:
			new_interval=interval;
	}
	GH_verbose(GH_MSG_INFO,"swap interval %d -> %d\n", interval, new_interval);

	return new_interval;
}

static int GH_swap_interval(int interval)
{
	static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
	static volatile GH_config cfg={GH_SWAP_MODES_COUNT, {0,1}};

	pthread_mutex_lock(&mutex);
	if (cfg.mode >= GH_SWAP_MODES_COUNT) {
		GH_config_from_str(&cfg, getenv("GH_SWAP_MODE"));
	}
	pthread_mutex_unlock(&mutex);
	return GH_swap_interval_for_cfg(&cfg, interval);
}


/***************************************************************************
 * FUNCTION INTERCEPTOR LOGIC                                              *
 ***************************************************************************/

typedef void (*GH_fptr)();
typedef void * (*GH_resolve_func)(const char *);

/* mutex used during GH_dlsym_internal () */
static pthread_mutex_t GH_mutex=PTHREAD_MUTEX_INITIALIZER;

/* Mutex for the function pointers. We only guard the
 * if (ptr == NULL) ptr=...; part. The pointers will never
 * change after being set to a non-NULL value for the first time,
 * so it is safe to dereference them without locking */
static pthread_mutex_t GH_fptr_mutex=PTHREAD_MUTEX_INITIALIZER;

/* THIS IS AN EVIL HACK: we directly call _dl_sym() of the glibc */
extern void *_dl_sym(void *, const char *, void (*)() );

/* Wrapper function called in place of dlsym(), since we intercept dlsym().
 * We use this ONLY to get the original dlsym() itself, all other symbol
 * resolutions are done via that original function, then.
 */
static void *GH_dlsym_internal(void *handle, const char *name)
{
	void *ptr;

	/* ARGH: we are bypassing glibc's locking for dlsym(), so we
	 * must do this on our own */
	pthread_mutex_lock(&GH_mutex);

	/* Third argument is the address of the caller, (glibc uses stack
	 * unwinding internally to get this),  we just use the address of our
	 * wrapper function itself, which is wrong when this is called on
	 * behalf of the real application doing a dlsycm, but we do not
	 *  care... */
	ptr=_dl_sym(handle, name, (void (*)())GH_dlsym_internal);

	pthread_mutex_unlock(&GH_mutex);
	return ptr;
}

/* Wrapper funtcion to query the original dlsym() function avoiding
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
static void (* volatile GH_glXSwapBuffers)(void *, unsigned long);
static void (* volatile GH_glXSwapIntervalEXT)(void *, unsigned long, int);
static int (* volatile GH_glXSwapIntervalSGI)(int);
static int (* volatile GH_glXSwapIntervalMESA)(int);
static void (*(* volatile GH_foo)(void))(const char *)=NULL;

/* Resolve an unintercepted symbol via the original dlsym() */
static void *GH_dlsym_next(const char *name)
{
	return GH_dlsym(RTLD_NEXT, name);
}

/* helper macro: query the symbol pointer if it is NULL
 * handle the locking */
#define GH_GET_PTR(func) \
	pthread_mutex_lock(&GH_fptr_mutex); \
	if(GH_ ##func == NULL) \
		GH_ ##func = GH_dlsym_next(#func);\
	pthread_mutex_unlock(&GH_fptr_mutex)

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
	pthread_mutex_lock(&GH_fptr_mutex); \
	if(GH_dlsym == NULL)
		GH_dlsym = GH_dlsym_internal_next("dlsym");
	pthread_mutex_unlock(&GH_fptr_mutex);
	interceptor=GH_get_interceptor(name, GH_dlsym_next, "dlsym");
	ptr=(interceptor)?interceptor:GH_dlsym(handle,name);
	GH_verbose(GH_MSG_DEBUG_INTERCEPTION,"dlsym(%p, %s) = %p%s\n",handle,name,ptr,
		interceptor?" [intercepted]":"");
	return ptr;
}

/* also intercept GNU specific dlvsym() */
extern void *
dlvsym(void *handle, const char *name, const char *version)
{
	void *interceptor;
	void *ptr;
	GH_GET_PTR(dlvsym); \
	interceptor=GH_get_interceptor(name, GH_dlsym_next, "dlsym");
	ptr=(interceptor)?interceptor:GH_dlvsym(handle,name,version);
	GH_verbose(GH_MSG_DEBUG_INTERCEPTION,"dlvsym(%p, %s, %s) = %p%s\n",handle,name,version,ptr,
		interceptor?" [intercepted]":"");
	return ptr;
}

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
extern GH_fptr procname(const char *name) \
{ \
	void *interceptor; \
	GH_fptr ptr; \
	GH_GET_PTR(procname); \
	interceptor=GH_get_interceptor(name, \
					(GH_resolve_func)GH_ ##procname, \
					 #procname); \
	ptr=(interceptor)?(GH_fptr)interceptor:GH_ ##procname(name); \
	GH_verbose(GH_MSG_DEBUG_INTERCEPTION,#procname "(%s) = %p%s\n",name, ptr, \
		interceptor?" [intercepted]":""); \
	return ptr; \
}

GH_GLXGETPROCADDRESS_GENERIC(glXGetProcAddress)
GH_GLXGETPROCADDRESS_GENERIC(glXGetProcAddressARB)

/* TARGET FUNCTION: glXSwapIntervalEXT */
extern void glXSwapIntervalEXT(void *dpy, unsigned long drawable,
				int interval)
{
	interval=GH_swap_interval(interval);
	if (interval < 1) {
		/* ignore the call */
		return;
	}
	GH_GET_PTR(glXSwapIntervalEXT);
	GH_glXSwapIntervalEXT(dpy, drawable, interval);
}

/* TARGET FUNCTION: glXSwapIntervalSGI */
extern int glXSwapIntervalSGI(int interval)
{
	interval=GH_swap_interval(interval);
	if (interval < 1) {
		/* ignore the call */
		return 0; /* success */
	}
	GH_GET_PTR(glXSwapIntervalSGI);
	return GH_glXSwapIntervalSGI(interval);
}

/* TARGET FUNCTION: glXSwapIntervalMESA */
extern int glXSwapIntervalMESA(int interval)
{
	interval=GH_swap_interval(interval);
	if (interval < 1) {
		/* ignore the call */
		return 0; /* success */
	}
	GH_GET_PTR(glXSwapIntervalMESA);
	return GH_glXSwapIntervalMESA(interval);
}

#ifndef NDEBUG
/* SwapBuffers is only intercepted for testing purposes */
extern void glXSwapBuffers(void *dpy, unsigned long drawable)
{
	GH_GET_PTR(glXSwapBuffers);
	GH_verbose(GH_MSG_INFO,"SwapBuffers\n");
	GH_glXSwapBuffers(dpy, drawable);
}
#endif

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

	GH_INTERCEPT(dlsym);
	GH_INTERCEPT(dlvsym);
	GH_INTERCEPT(glXGetProcAddress);
	GH_INTERCEPT(glXGetProcAddressARB);
	GH_INTERCEPT(glXSwapIntervalEXT);
	GH_INTERCEPT(glXSwapIntervalSGI);
#ifndef NDEBUG
	GH_INTERCEPT(glXSwapBuffers);
#endif
	return NULL;
}

