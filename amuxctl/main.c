#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "amuxctl.h"
#include "opt.h"

int main(int argc, char *argv[])
{
	struct am_opt opt;
	struct amux_ctx *actx = NULL;
	char pcm[256];
	int ret;

	ret = parse_args(&opt, argc, argv);
	if(ret != 0) {
		goto out;
	}

	actx = amux_ctx_new();
	if(actx == NULL) {
		goto out;
	}

	ret = amux_ctx_init(actx);
	if(ret != 0)
		goto out;

	switch(opt.act) {
	case AA_LIST:
		amux_pcmlst_dump(actx);
		break;
	case AA_SET:
		ret = amux_pcm_set(actx, opt.sopt.pcm);
		if(ret != 0)
			fprintf(stderr, "Can't set PCM: %s\n", strerror(-ret));
		break;
	case AA_GET:
		ret = amux_pcm_get(actx, pcm, sizeof(pcm));
		if(ret < 0)
			fprintf(stderr, "Can't get PCM: %s\n", strerror(-ret));
		printf("Current PCM: %.*s\n", ret, pcm);
		break;
	default:
		break;
	}

	amux_ctx_cleanup(actx);
out:
	if(actx) {
		amux_ctx_free(actx);
	}
	return ret;
}
