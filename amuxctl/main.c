#include <stdlib.h>
#include <stdio.h>
#include <sys/file.h>

#include <alsa/asoundlib.h>

#include "pcmlist.h"
#include "opt.h"

struct amux_ctx {
	snd_config_t *top;
	char const *file;
	struct pcmlst plst;
};

static char const *cfg_get_str(snd_config_t *cfg, char const *key)
{
	char const *str = NULL;
	snd_config_t *entry;
	int ret;

	ret = snd_config_search(cfg, key, &entry);
	if(ret < 0) {
		fprintf(stderr, "Cannot get default cfg node for %s\n", key);
		goto out;
	}

	ret = snd_config_get_string(entry, &str);
	if(ret < 0) {
		fprintf(stderr, "Cannot get default cfg value for %s\n", key);
		str = NULL;
		goto out;
	}
out:
	return str;
}

static int amux_cfg_parse(struct amux_ctx *ctx)
{
	snd_config_t *dft;
	char const *str;
	int ret;

	ctx->top = snd_config;
	snd_config_ref(ctx->top);

	ret = snd_config_search(ctx->top, "pcm.default", &dft);
	if(ret < 0) {
		fprintf(stderr, "Cannot get default config\n");
		goto err;
	}

	ret = -EINVAL;
	str = cfg_get_str(dft, "type");
	if((str == NULL) || (strcmp(str, "amux") != 0))
		goto unref;

	str = cfg_get_str(dft, "file");
	if(str == NULL)
		goto unref;

	ctx->file = str;
	return 0;
unref:
	snd_config_unref(ctx->top);
	snd_config_update_free_global();
err:
	return ret;
}

static int amux_pcm_set(struct amux_ctx *actx, char const *pcm)
{
	size_t totsz, cursz;
	ssize_t sz;
	int fd = -1, ret;

	ret = open(actx->file, O_WRONLY);
	if(ret < 0)
		goto out;

	fd = ret;
	totsz = strlen(pcm);
	cursz = 0;

	ret = flock(fd, LOCK_EX);
	if(ret < 0) {
		ret = -errno;
		goto close;
	}

	while(cursz < totsz) {
		sz = write(fd, pcm + cursz, totsz - cursz);
		if(sz <= 0) {
			ret = (int)sz;
			goto unlock;
		}
		cursz += sz;
	}

	ret = ftruncate(fd, totsz);

unlock:
	flock(fd, LOCK_UN);
close:
	close(fd);
out:
	return ret;
}

static int amux_pcm_get(struct amux_ctx *actx, char *pcm, size_t len)
{
	size_t cursz;
	ssize_t sz;
	int fd = -1, ret;

	ret = open(actx->file, O_RDONLY);
	if(ret < 0)
		goto out;

	fd = ret;
	cursz = 0;

	ret = flock(fd, LOCK_SH);
	if(ret < 0) {
		ret = -errno;
		goto close;
	}

	sz = 1;
	while(sz != 0) {
		sz = read(fd, pcm + cursz, len - cursz);
		if(sz < 0) {
			ret = (int)sz;
			goto unlock;
		}
		cursz += sz;
	}
	ret = cursz;

unlock:
	flock(fd, LOCK_UN);
close:
	close(fd);
out:
	return ret;
}

static int amux_ctx_init(struct amux_ctx *actx)
{
	int ret;

	ret = pcmlst_init(&actx->plst);
	if(ret != 0) {
		fprintf(stderr, "Cannot initialize PCM list\n");
		goto err;
	}

	ret = amux_cfg_parse(actx);
	if(ret != 0)
		goto clean;

	return 0;
clean:
	pcmlst_cleanup(&actx->plst);
err:
	return ret;
}

static void amux_ctx_cleanup(struct amux_ctx *actx)
{
	snd_config_unref(actx->top);
	/* Cleanup alsalib mess */
	snd_config_update_free_global();
	pcmlst_cleanup(&actx->plst);
}

int main(int argc, char *argv[])
{
	struct am_opt opt;
	struct amux_ctx actx;
	char pcm[256];
	int ret;

	ret = parse_args(&opt, argc, argv);
	if(ret != 0) {
		goto out;
	}

	ret = amux_ctx_init(&actx);
	if(ret != 0)
		goto out;

	switch(opt.act) {
	case AA_LIST:
		pcmlst_dump(&actx.plst);
		break;
	case AA_SET:
		ret = amux_pcm_set(&actx, opt.sopt.pcm);
		if(ret != 0)
			fprintf(stderr, "Can't set PCM: %s\n", strerror(-ret));
		break;
	case AA_GET:
		ret = amux_pcm_get(&actx, pcm, sizeof(pcm));
		if(ret < 0)
			fprintf(stderr, "Can't get PCM: %s\n", strerror(-ret));
		printf("Current PCM: %.*s\n", ret, pcm);
		break;
	default:
		break;
	}

	amux_ctx_cleanup(&actx);
out:
	return ret;
}
