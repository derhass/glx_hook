CFLAGS += -Wall

ifeq ($(DEBUG), 1)
CFLAGS += -g -Werror
else
CFLAGS += -O2 -DNDEBUG
endif

glx_hook.so: glx_hook.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -shared -fPIC -Bsymbolic -pthread -o $@ $<

.PHONY: clean
clean: 
	-rm glx_hook.so

