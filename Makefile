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
	$(CC) $(CPPFLAGS) $(STDDEFINES) $(CFLAGS) $(LDFLAGS) -shared -fPIC -Bsymbolic -pthread -o $@ $<
glx_hook_bare.so: glx_hook.c
	$(CC) $(CPPFLAGS) $(BAREDEFINES) $(CFLAGS) $(LDFLAGS) -shared -fPIC -Bsymbolic -pthread -o $@ $<

.PHONY: clean
clean: 
	-rm glx_hook.so

