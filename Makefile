CPPFLAGS += -Wall -Wextra

ifeq ($(DEBUG), 1)
CFLAGS += -g
CPPFLAGS += -Werror
else
CFLAGS += -O2
CPPFLAGS += -DNDEBUG
endif

ifeq ($(METHOD),)
METHOD = 2
endif
CPPFLAGS += -D GH_DLSYM_METHOD=$(METHOD)

STDDEFINES=-DGH_CONTEXT_TRACKING -DGH_SWAPBUFFERS_INTERCEPT
BAREDEFINES=

BASEFILES=glx_hook.so glx_hook_bare.so

.PHONY: all
ifeq ($(METHOD),3)
all: $(BASEFILES) dlsym_wrapper.so
else
all: $(BASEFILES)
endif

glx_hook.so: glx_hook.c dlsym_wrapper.h Makefile
	$(CC)  -shared -fPIC -Bsymbolic -pthread -o $@ $< $(CPPFLAGS) $(STDDEFINES) $(CFLAGS) $(LDFLAGS) -lrt
glx_hook_bare.so: glx_hook.c dlsym_wrapper.h Makefile
	$(CC)  -shared -fPIC -Bsymbolic -pthread -o $@ $< $(CPPFLAGS) $(BAREDEFINES) $(CFLAGS) $(LDFLAGS)
dlsym_wrapper.so: dlsym_wrapper.c dlsym_wrapper.h Makefile
	$(CC)  -shared -fPIC -Bsymbolic -o $@ $< $(CPPFLAGS) $(BAREDEFINES) $(CFLAGS) $(LDFLAGS) -ldl

.PHONY: clean
clean: 
	-rm $(BASEFILES) dlsym_wrapper.so

