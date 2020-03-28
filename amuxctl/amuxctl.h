#ifndef _AMUXCTL_H_
#define _AMUXCTL_H_

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct amux_ctx;

struct amux_ctx *amux_ctx_new();

int amux_ctx_init(struct amux_ctx *actx);

void amux_pcmlst_dump(struct amux_ctx *actx);

int amux_pcm_set(struct amux_ctx *actx, char const *pcm);

int amux_pcm_get(struct amux_ctx *actx, char *pcm, size_t len);

void amux_ctx_cleanup(struct amux_ctx *actx);

void amux_ctx_free(struct amux_ctx *actx);

#ifdef __cplusplus
}
#endif

#endif
