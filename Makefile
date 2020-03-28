CC=gcc
CFLAGS= -W -Wall -DPIC -fPIC -O0 -g -I include
LDFLAGS=

BUILDDIR=build

# Amux library
AML_SRCDIR=src
AML_BUILDDIR=$(BUILDDIR)/aml
AML_SRC= amux.c poller/poller.c poller/dupfd.c poller/thread.c \
	poller/epoller.c
AML_OBJ=$(AML_SRC:%.c=$(AML_BUILDDIR)/%.o)
AML_DEPEND=$(AML_SRC:%.c=$(AML_BUILDDIR)/%.d)
AML_LDFLAGS= -lasound -T $(AML_SRCDIR)/script.ld
AML=$(if $(AML_SRC),$(BUILDDIR)/libasound_pcm_amux.so)

# Amux control program
ACTL_SRCDIR=amuxctl
ACTL_BUILDDIR=$(BUILDDIR)/actl
ACTL_LIB_SRC=amuxctl.c pcmlist.c
ACTL_LIB_OBJ=$(ACTL_LIB_SRC:%.c=$(ACTL_BUILDDIR)/%.o)
ACTL_LIB_DEPEND=$(ACTL_LIB_SRC:%.c=$(ACTL_BUILDDIR)/%.d)
ACTL_LIB_LDFLAGS= -lasound
ACTL_LIB=$(if $(ACTL_LIB_SRC),$(BUILDDIR)/libamuxctl.so)
ACTL_BIN_SRC=main.c opt.c
ACTL_BIN_OBJ=$(ACTL_BIN_SRC:%.c=$(ACTL_BUILDDIR)/%.o)
ACTL_BIN_DEPEND=$(ACTL_BIN_SRC:%.c=$(ACTL_BUILDDIR)/%.d)
ACTL_BIN_LDFLAGS= -L$(BUILDDIR) -lamuxctl
ACTL_BIN=$(if $(ACTL_BIN_SRC),$(BUILDDIR)/amuxctl)

ifeq ($(DEBUG),1)
ACTL_CFLAGS+=-ggdb -fno-omit-frame-pointer -fsanitize=address -fsanitize=leak
ACTL_LDFLAGS:=-lasan $(ACTL_LDFLAGS)
endif

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

all: $(AML) $(ACTL_LIB) $(ACTL_BIN)

$(AML): $(AML_OBJ)
	gcc -shared -o $@ $^ $(LDFLAGS) $(AML_LDFLAGS)

$(ACTL_LIB): $(ACTL_LIB_OBJ)
	gcc -shared -o $@ $^ $(LDFLAGS) $(ACTL_LIB_LDFLAGS)

$(ACTL_BIN): $(ACTL_BIN_OBJ) $(ACTL_LIB)
	gcc -o $@ $^ $(LDFLAGS) $(ACTL_BIN_LDFLAGS)

$(AML_BUILDDIR)/%.o: $(AML_SRCDIR)/%.c
	@mkdir -p $(dir $(@))
	gcc -MMD -c -o $@ $< $(CFLAGS) $(AML_CFLAGS)

$(ACTL_BUILDDIR)/%.o: $(ACTL_SRCDIR)/%.c
	@mkdir -p $(dir $(@))
	gcc -MMD -c -o $@ $< $(CFLAGS) $(ACTL_CFLAGS)

amlclean:
	$(call rm-file,$(AML_OBJ))
	$(call rm-file,$(AML_DEPEND))
	$(call rm-dir,$(call reverse,$(dir $(AML_OBJ))))
	$(call rm-dir,$(AML_BUILDDIR))

actlclean:
	$(call rm-file,$(ACTL_LIB_OBJ))
	$(call rm-file,$(ACTL_LIB_DEPEND))
	$(call rm-dir,$(call reverse,$(dir $(ACTL_LIB_OBJ))))
	$(call rm-file,$(ACTL_BIN_OBJ))
	$(call rm-file,$(ACTL_BIN_DEPEND))
	$(call rm-dir,$(call reverse,$(dir $(ACTL_BIN_OBJ))))
	$(call rm-dir,$(ACTL_BUILDDIR))

amldistclean: amlclean
	$(call rm-file,$(AML))

actldistclean: actlclean
	$(call rm-file,$(ACTL_LIB))
	$(call rm-file,$(ACTL_BIN))

.PHONY: clean

clean: amlclean actlclean

distclean: amldistclean actldistclean

-include $(AML_DEPEND)
-include $(ACTL_DEPEND)
