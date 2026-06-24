CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

all: wine_dll_redirect.so

wine_dll_redirect.so: wine_dll_redirect.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl -pthread $(LDFLAGS)

clean:
	rm -f wine_dll_redirect.so

.PHONY: all clean
