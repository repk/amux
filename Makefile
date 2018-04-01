CC=gcc
CFLAGS= -W -Wall -DPIC -fPIC -O0 -g
LDFLAGS= -lasound

SRCDIR=src
BUILDDIR=build
SRC=amux.c
OBJ=$(SRC:%.c=$(BUILDDIR)/%.o)
DEPEND=$(SRC:%.c=$(BUILDDIR)/%.d)
LIBRARY=$(BUILDDIR)/libasound_pcm_amux.so

all: $(LIBRARY)

$(LIBRARY): $(OBJ)
	gcc -shared -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $(@))
	gcc -MMD -c -o $@ $< $(CFLAGS)

.PHONY: clean

clean:
	rm -f $(OBJ)
	rm -f $(DEPEND)

distclean: clean
	rm -f $(LIBRARY)
	(rmdir $(dir $(OBJ)) > /dev/null 2>&1) || true
	(rmdir $(BUILDDIR) > /dev/null 2>&1) || true

-include $(DEPEND)
