#ifndef DLSYM_WRAPPER_H
#define DLSYM_WRAPPER_H

#define DLSYM_WRAPPER_ENVNAME "DLSYM_WRAPPER_ORIG_FPTR"
#define DLSYM_WRAPPER_NAME "dlsym_wrapper.so"
typedef void* (*DLSYM_PROC_T)(void*, const char*);

#endif
