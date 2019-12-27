CC=gcc
CFLAGS= -W -Wall -DPIC -fPIC -O0 -g -I include
LDFLAGS= -lasound -T $(SRCDIR)/script.ld

SRCDIR=src
BUILDDIR=build
SRC=amux.c poller/poller.c poller/dupfd.c poller/thread.c
OBJ=$(SRC:%.c=$(BUILDDIR)/%.o)
DEPEND=$(SRC:%.c=$(BUILDDIR)/%.d)
LIBRARY=$(BUILDDIR)/libasound_pcm_amux.so

define _reverse
$(if $(1),\
	$(call _reverse,$(wordlist 2,$(words $(1)),$(1)))\
	$(firstword $(1))\
)
endef

define reverse
$(strip $(call _reverse,$(sort $(1))))
endef

define rm-file
$(if $(1), rm -f $(1))
endef

define rm-dir
$(if $(1), (rmdir $(1) > /dev/null 2>&1) || true)
endef

all: $(LIBRARY)

$(LIBRARY): $(OBJ)
	gcc -shared -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $(@))
	gcc -MMD -c -o $@ $< $(CFLAGS)

.PHONY: clean

clean:
	$(call rm-file,$(OBJ))
	$(call rm-file,$(DEPEND))
	$(call rm-dir,$(call reverse,$(dir $(OBJ))))
	$(call rm-dir,$(BUILDDIR))

distclean: clean
	$(call rm-file,$(LIBRARY))

-include $(DEPEND)
