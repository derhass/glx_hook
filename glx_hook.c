#define _GNU_SOURCE
#include <dlfcn.h>	/* for RTLD_NEXT */
#include <pthread.h>	/* for mutextes */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>

#include <GL/glx.h>
#include <GL/glxext.h>

/***************************************************************************
 * helpers                                                                 *
 ***************************************************************************/

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

/* function pointers we just might qeury */
static void (* volatile GH_glFlush)(void);
static void (* volatile GH_glFinish)(void);

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
 * GL context tracking                                                     *
 ***************************************************************************/

#ifdef GH_CONTEXT_TRACKING

typedef struct gl_context_s {
	GLXContext ctx;
	GLXDrawable draw;
	GLXDrawable read;
	unsigned int flags;
	int swapbuffers;
	int swapbuffer_cnt;
	struct gl_context_s *next;
} gl_context_t;

/* flag bits */
#define GH_GL_CURRENT		0x1

static gl_context_t * volatile ctx_list=NULL;
static volatile int ctx_firsttime=1;
static pthread_mutex_t ctx_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t ctx_current;

static void
add_ctx(gl_context_t *glc)
{
	pthread_mutex_lock(&ctx_mutex);
	glc->next=ctx_list;
	ctx_list=glc;
	pthread_mutex_unlock(&ctx_mutex);
}

static gl_context_t *
create_ctx(GLXContext ctx)
{
	gl_context_t *glc;

	glc=malloc(sizeof(*glc));
	if (glc) {
		glc->ctx=ctx;
		glc->draw=None;
		glc->read=None;
		glc->swapbuffers=0;
		glc->swapbuffer_cnt=0;
		glc->flags=0;
	}
	return glc;
}

static void
destroy_ctx(gl_context_t *glc)
{
	free(glc);
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
	glc->swapbuffers=get_envi("GH_SWAPBUFFERS",-1);
}

static void
create_context(GLXContext ctx)
{
	gl_context_t *glc;

	GH_verbose(GH_MSG_DEBUG, "created ctx %p\n",ctx);

	pthread_mutex_lock(&ctx_mutex);
	if (ctx_firsttime) {
		ctx_firsttime=0;
		pthread_key_create(&ctx_current, NULL);
		pthread_setspecific(ctx_current, NULL);
		/* query the function pointers for the standad function
		 * which might be often called ... */
		GH_GET_PTR(glXSwapBuffers);
		GH_GET_PTR(glXMakeCurrent);
		GH_GET_PTR(glXMakeContextCurrent);
		GH_GET_PTR(glXMakeCurrentReadSGI);
		GH_GET_PTR(glFlush);
		GH_GET_PTR(glFinish);
	}
	pthread_mutex_unlock(&ctx_mutex);
	
	glc=create_ctx(ctx);
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
make_current(GLXContext ctx, GLXDrawable draw, GLXDrawable read)
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
		}
	} else {
		glc=NULL;
	}

	pthread_setspecific(ctx_current, glc);
}

#endif /* GH_CONTEXT_TRACKING */

/***************************************************************************
 * SWAP INTERVAL LOGIC                                                     *
 ***************************************************************************/

/* we use this value as swap interval to mark the situation that we should
 * not set the swap interval at all. Note that with 
 * GLX_EXT_swap_control_tear, negative intervals are allowed, and the 
 * absolute value specifies the real interval, so we use just INT_MIN to 
 * avoid conflicts with values an application might set. */
#define GH_SWAP_DONT_SET	INT_MIN

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

	GH_verbose(GH_MSG_INFO,"swap interval %d -> %d\n", interval, new_interval);
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

	GH_GET_PTR(glXCreateContext);
	ctx=GH_glXCreateContext(dpy, vis, shareList, direct);
	create_context(ctx);
	return ctx;
}

extern GLXContext glXCreateNewContext( Display *dpy, GLXFBConfig config, int renderType, GLXContext shareList, Bool direct )
{
	GLXContext ctx;

	GH_GET_PTR(glXCreateNewContext);
	ctx=GH_glXCreateNewContext(dpy, config, renderType, shareList, direct);
	create_context(ctx);
	return ctx;
}

extern GLXContext glXCreateContextAttribsARB (Display * dpy, GLXFBConfig config, GLXContext shareList, Bool direct, const int *attr)
{
	GLXContext ctx;

	GH_GET_PTR(glXCreateContextAttribsARB);
	ctx=GH_glXCreateContextAttribsARB(dpy, config, shareList, direct, attr);
	create_context(ctx);
	return ctx;
}

extern GLXContext glXImportContextEXT (Display *dpy, GLXContextID id)
{
	GLXContext ctx;

	GH_GET_PTR(glXImportContextEXT);
	ctx=GH_glXImportContextEXT(dpy, id);
	create_context(ctx);
	return ctx;
}

extern GLXContext glXCreateContextWithConfigSGIX (Display *dpy, GLXFBConfigSGIX config, int renderType, GLXContext shareList, Bool direct)
{
	GLXContext ctx;

	GH_GET_PTR(glXCreateContextWithConfigSGIX);
	ctx=GH_glXCreateContextWithConfigSGIX(dpy, config, renderType, shareList, direct);
	create_context(ctx);
	return ctx;
}

/* ---------- Context Destruction ---------- */

extern void glXDestroyContext(Display *dpy, GLXContext ctx)
{
	GH_GET_PTR(glXDestroyContext);
	GH_glXDestroyContext(dpy, ctx);
	destroy_context(ctx);
}

extern void glXFreeContextEXT(Display *dpy, GLXContext ctx)
{
	GH_GET_PTR(glXFreeContextEXT);
	GH_glXFreeContextEXT(dpy, ctx);
	destroy_context(ctx);
}

/* ---------- Current Context Tracking ---------- */

extern Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
	Bool result;

	result=GH_glXMakeCurrent(dpy, drawable, ctx);
	make_current(ctx, drawable, drawable);
	return result;
}

extern Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
	Bool result;

	result=GH_glXMakeContextCurrent(dpy, draw, read, ctx);
	make_current(ctx, draw, read);
	return result;
}

extern Bool glXMakeCurrentReadSGI(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
	Bool result;

	result=GH_glXMakeCurrentReadSGI(dpy, draw, read, ctx);
	make_current(ctx, draw, read);
	return result;
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
	GH_GET_PTR(glXSwapIntervalEXT);
	GH_glXSwapIntervalEXT(dpy, drawable, interval);
}

extern int glXSwapIntervalSGI(int interval)
{
	interval=GH_swap_interval(interval);
	if (interval == GH_SWAP_DONT_SET) {
		/* ignore the call */
		return 0; /* success */
	}
	GH_GET_PTR(glXSwapIntervalSGI);
	return GH_glXSwapIntervalSGI(interval);
}

extern int glXSwapIntervalMESA(unsigned int interval)
{
	interval=GH_swap_interval((int)interval);
	if (interval == GH_SWAP_DONT_SET) {
		/* ignore the call */
		return 0; /* success */
	}
	GH_GET_PTR(glXSwapIntervalMESA);
	return GH_glXSwapIntervalMESA(interval);
}

/* ---------- Swap Buffers ---------- */

#ifdef GH_SWAPBUFFERS_INTERCEPT
extern void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
#ifdef GH_CONTEXT_TRACKING
	gl_context_t *glc=(gl_context_t*)pthread_getspecific(ctx_current);

	if (glc) {
		if (glc->swapbuffers >= 0) {
			if (++glc->swapbuffer_cnt==glc->swapbuffers) {
				GH_glXSwapBuffers(dpy, drawable);
				glc->swapbuffer_cnt=0;
			} else {
				/* GH_glFinish(); */
				GH_glFlush();
			}
		} else {
			GH_GET_PTR(glXSwapBuffers);
			GH_glXSwapBuffers(dpy, drawable);
		}
	} else {
		GH_verbose(GH_MSG_WARNING,"SwapBuffers called without a context\n");
		GH_GET_PTR(glXSwapBuffers);
		GH_glXSwapBuffers(dpy, drawable);
	}
#else /* GH_CONTEXT_TRACKING */
	GH_GET_PTR(glXSwapBuffers);
	GH_glXSwapBuffers(dpy, drawable);
#endif /* GH_CONTEXT_TRACKING */
}
#endif /* GH_SWAPBUFFERS_INTERCEPT */

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
	static int do_swapbuffers=-1;
#endif

	GH_INTERCEPT(dlsym);
	GH_INTERCEPT(dlvsym);
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
#endif
#ifdef GH_SWAPBUFFERS_INTERCEPT
	if (do_swapbuffers) {
		if (do_swapbuffers < 0)
			do_swapbuffers=get_envi("GH_SWAPBUFFERS", 0);
		if (do_swapbuffers)
			GH_INTERCEPT(glXSwapBuffers);
	}
#endif
	return NULL;
}

