#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <poll.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "amux.h"
#include "poller/poller.h"

/**
 * Find a registered poller description.
 *
 * @param name: Name of poller desc to find.
 * @return: Found poller desc on succes, NULL otherwise.
 */
static struct poller_desc const *poller_find(char const *name)
{
	struct poller_desc const * const *p;
	struct poller_desc const *ret = NULL;
	extern struct poller_desc const *__poller_start;
	extern struct poller_desc const *__poller_end;

	for(p = &__poller_start; p < &__poller_end; ++p) {
		if(strcmp((*p)->name, name) == 0) {
			ret = *p;
			break;
		}
	}

	return ret;
}

/**
 * Create a new poller instance.
 *
 * @param amx: Amux PCM master
 * @param name: Name of the poller to create
 * @param args: Arguments to pass at poller creation
 * @return: New poller instance on success, negative number otherwise
 */
struct poller *poller_create(struct snd_pcm_amux *amx, char const *name,
		void *args)
{
	struct poller_desc const *desc;
	struct poller *ret = NULL;
	int err;

	AMUX_DBG("%s: enter\n", __func__);

	desc = poller_find(name);
	if(desc == NULL) {
		AMUX_ERR("%s: Invalid poller name \"%s\"\n", __func__, name);
		goto out;
	}

	AMUX_ASSERT(desc->ops->create);

	err = desc->ops->create(&ret, args);
	if(err != 0) {
		ret = NULL;
		AMUX_ERR("%s: Poller creation error\n", __func__);
		goto out;
	}

	ret->desc = desc;
	ret->amx = amx;
out:
	return ret;
}

/**
 * Destroy a poller instance.
 *
 * @param p: poller instance to destroy
 */
void poller_destroy(struct poller *p)
{
	AMUX_DBG("%s: enter\n", __func__);
	AMUX_ASSERT(p->desc->ops->destroy != NULL);
	p->desc->ops->destroy(p);
}

/**
 * Set poller current PCM slave.
 *
 * @param p: poller instance
 * @param slave: new slave
 * @return: 0 on success, negative number otherwise
 */
int poller_set_slave(struct poller *p)
{
	AMUX_DBG("%s: enter\n", __func__);
	AMUX_ASSERT(p->desc->ops->set_slave != NULL);
	return p->desc->ops->set_slave(p);
}

/**
 * Get poller number of file descriptors to poll for.
 *
 * @param p: poller instance
 * @return: number of file descriptors
 */
int poller_descriptors_count(struct poller *p)
{
	AMUX_DBG("%s: enter\n", __func__);
	AMUX_ASSERT(p->desc->ops->descriptors_count != NULL);
	return p->desc->ops->descriptors_count(p);
}

/**
 * Fillup an pollfd array with file descriptors to poll
 *
 * @param p: poller instance
 * @param pfd: Array to fill
 * @param nr: Size of array
 * @return: the number of fd on success, negative number otherwise
 */
int poller_descriptors(struct poller *p, struct pollfd *pfd, size_t nr)
{
	AMUX_DBG("%s: enter\n", __func__);
	AMUX_ASSERT(p->desc->ops->descriptors != NULL);
	return p->desc->ops->descriptors(p, pfd, nr);
}

/**
 * Demangle poll result events
 *
 * @param p: poller instance
 * @param pfd: Array of pollfd to demangle
 * @param nr: Size of array
 * @param revents: Demangled result poll events
 * @return: 0 on success, negative number otherwise
 */
int poller_poll_revents(struct poller *p, struct pollfd *pfd, size_t nr,
		unsigned short *revents)
{
	AMUX_DBG("%s: enter\n", __func__);
	AMUX_ASSERT(p->desc->ops->poll_revents != NULL);
	return p->desc->ops->poll_revents(p, pfd, nr, revents);
}

/**
 * Notify new data transfer
 *
 * @param p: Common poller poll thread instance
 */
void poller_transfer(struct poller *p)
{
	AMUX_DBG("%s: enter\n", __func__);
	if(p->desc->ops->transfer != NULL)
		p->desc->ops->transfer(p);
}
