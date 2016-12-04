CFLAGS += -Wall

ifeq ($(DEBUG), 1)
CFLAGS += -g -Werror
else
CFLAGS += -O2 -DNDEBUG
endif

STDDEFINES=-DGH_CONTEXT_TRACKING -DGH_SWAPBUFFERS_INTERCEPT
BAREDEFINES=

.PHONY: all
all: glx_hook.so glx_hook_bare.so

glx_hook.so: glx_hook.c
	$(CC)  -shared -fPIC -Bsymbolic -pthread -o $@ $< $(CPPFLAGS) $(STDDEFINES) $(CFLAGS) $(LDFLAGS) -lrt
glx_hook_bare.so: glx_hook.c
	$(CC)  -shared -fPIC -Bsymbolic -pthread -o $@ $< $(CPPFLAGS) $(STDDEFINES) $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean: 
	-rm glx_hook.so glx_hook_bare.so

