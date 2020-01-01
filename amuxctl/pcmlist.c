#include <stdlib.h>
#include <stdio.h>
#include <alsa/asoundlib.h>

#include "pcmlist.h"

struct pcm {
	LIST_ENTRY(pcm) next;
	char *name;
	char *desc;
};

static int pcm_init(struct pcm *pcm, char *name, char *desc)
{
	pcm->name = name;
	pcm->desc = desc;
	return 0;
}

static void pcm_cleanup(struct pcm *pcm)
{
	if(pcm->name)
		free(pcm->name);
	if(pcm->desc)
		free(pcm->desc);
}

void pcmlst_dump(struct pcmlst const *plst)
{
	struct pcm *pcm;

	LIST_FOREACH(pcm, plst, next)
		printf("PCM %s:\n%s\n", pcm->name, pcm->desc);
}

int pcmlst_add(struct pcmlst *plst, void const *hint)
{
	struct pcm *pcm;
	char *name, *desc, *io;
	int ret = -EINVAL;

	name = snd_device_name_get_hint(hint, "NAME");
	desc = snd_device_name_get_hint(hint, "DESC");

	io = snd_device_name_get_hint(hint, "IOID");
	if (io != NULL && strcmp(io, "Output") != 0)
		goto err;
	if(io)
		free(io);

	if((name == NULL) || (desc == NULL))
		goto err;

	pcm = malloc(sizeof(*pcm));
	if(pcm == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	ret = pcm_init(pcm, name, desc);
	if(ret != 0) {
		free(pcm);
		goto err;
	}
	LIST_INSERT_HEAD(plst, pcm, next);

	return 0;
err:
	if(name)
		free(name);
	if(desc)
		free(desc);
	return ret;
}

int pcmlst_init(struct pcmlst *plst)
{
	void **hints, **h;
	int ret;

	LIST_INIT(plst);
	ret = snd_device_name_hint(-1, "pcm", &hints);
	if(ret < 0)
		return ret;

	for(h = hints; *h != NULL; ++h) {
		ret = pcmlst_add(plst, *h);
		if(ret == -ENOMEM)
			break;
	}

	snd_device_name_free_hint(hints);
	return ret;
}

void pcmlst_cleanup(struct pcmlst *plst)
{
	struct pcm *pcm;
	while((pcm = LIST_FIRST(plst)) != NULL) {
		pcm_cleanup(pcm);
		LIST_REMOVE(pcm, next);
		free(pcm);
	}
}
