CC=gcc
CFLAGS= -W -Wall -DPIC -fPIC -O0 -g
LDFLAGS= -lasound

SRCDIR=src
BUILDDIR=build
SRC=amux.c
OBJ=$(SRC:%.c=$(BUILDDIR)/%.o)
LIBRARY=$(BUILDDIR)/libasound_pcm_amux.so

all: $(LIBRARY)

$(LIBRARY): $(OBJ)
	gcc -shared -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	gcc -c -o $@ $^ $(CFLAGS)

.PHONY: clean

clean:
	rm -f $(BUILDDIR)/*.o

distclean: clean
	rm -f $(LIBRARY)
	(rmdir $(BUILDDIR) > /dev/null 2>&1) || true
