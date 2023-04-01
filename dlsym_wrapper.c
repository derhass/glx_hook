#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "dlsym_wrapper.h"

__attribute__((constructor))
static void dlsym_wrapper_init()
{
	if (getenv(DLSYM_WRAPPER_ENVNAME) == NULL) {
		/* big enough to hold our pointer as hex string, plus a NUL-terminator */
		char buf[sizeof(DLSYM_PROC_T)*2 + 3];
		DLSYM_PROC_T dlsym_ptr=dlsym;
		if (snprintf(buf, sizeof(buf), "%p", dlsym_ptr) < (int)sizeof(buf)) {
			buf[sizeof(buf)-1] = 0;
			if (setenv(DLSYM_WRAPPER_ENVNAME, buf, 1)) {
				fprintf(stderr,"dlsym_wrapper: failed to set '%s' to '%s'\n", DLSYM_WRAPPER_ENVNAME, buf);
			}
		} else {
			fprintf(stderr,"dlsym_wrapper: failed to encode 0x%p in buffer\n", dlsym_ptr);
		}
	} else {
		fprintf(stderr,"dlsym_wrapper: '%s' already set\n", DLSYM_WRAPPER_ENVNAME);
	}
}
