#!/bin/sh

CURDIR=$(dirname $(realpath ${0}))

LD_LIBRARY_PATH=${CURDIR}/../../alsa-lib-1.1.4.1/src/.libs/ \
ALSA_CONFIG_PATH=${CURDIR}/asoundrc \
AMUX_LIBRARY=${CURDIR}/../build/libasound_pcm_amux.so \
gdb --args ../alsa-utils-1.1.4/aplay/aplay --dump-hw-params -M -v -f cd "$@"
#gdb --args mpv "$@"
#valgrind --leak-check=full --show-leak-kinds=all ../alsa-utils-1.1.4/aplay/aplay --dump-hw-params -M -v -f cd "$@"
#firefox -P vpn --no-remote --new-instance
