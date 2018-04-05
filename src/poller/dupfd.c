#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "amux.h"
#include "poller/poller.h"

#define DUPFD_POLLFD_MAX 4

/**
 * Dupfd poller structure
 */
struct dupfd {
	/**
	 * poller common structure
	 */
	struct poller p;
	/**
	 * Mock poll file descriptor arrays.
	 * These arrays are used to present a constant poll interface to user.
	 * When slave changes, those file descriptors are set to point to the
	 * new slave's fds, but their file descriptor numbers do not change.
	 * Thus making the slave switch operation transparent for user.
	 *
	 * infd is for descriptors to poll for POLLIN events.
	 */
	int infd[DUPFD_POLLFD_MAX];
	/**
	 * outfd is for descriptors to poll for POLLOUT events.
	 */
	int outfd[DUPFD_POLLFD_MAX];
	/**
	 * efd is used to have always blocking fd. Use efd[1] as a fd
	 * placeholder in infd and efd[0] as a placeholder for outfd.
	 */
	int efd[2];
};
#define to_dupfd(poller) (container_of(poller, struct dupfd, p))

/**
 * Initialize a dupfd structure
 *
 * @param d: dupfd to initialize
 * @return: 0 on success, negative number otherwise
 */
static inline int dupfd_init(struct dupfd *d)
{
	size_t i;
	int ret;
	ret = pipe(d->efd);
	if(ret < 0) {
		AMUX_ERR("%s: Pipe creation error\n", __func__);
		ret = -errno;
		goto err;
	}

	for(i = 0; i < DUPFD_POLLFD_MAX; ++i) {
		d->infd[i] = dup(d->efd[1]);
		if(d->infd[i] < 0)
			goto dupclose;

		d->outfd[i] = dup(d->efd[0]);
		if(d->outfd[i] < 0)
			goto dupclose;
	}

	return 0;

dupclose:
	ret = -errno;
	/* close all successful dup() */
	for(i = 0; i < DUPFD_POLLFD_MAX; ++i) {
		if(d->infd[i] < 0)
			break;
		close(d->infd[i]);
		if(d->outfd[i] < 0)
			break;
		close(d->outfd[i]);
	}
err:
	return ret;
}

/**
 * Cleanup a dupfd structure
 *
 * @param d: dupfd to cleanup
 */
static inline void dupfd_cleanup(struct dupfd *d)
{
	size_t i;
	close(d->efd[0]);
	close(d->efd[1]);
	for(i = 0; i < DUPFD_POLLFD_MAX; ++i) {
		close(d->infd[i]);
		close(d->outfd[i]);
	}
}

/**
 * Return the number of file descriptors to poll.
 *
 * @param p: Common poller dupfd instance
 * @return: the number of file descriptors
 */
static int dupfd_descriptors_count(struct poller *p)
{
	(void)p;
	return DUPFD_POLLFD_MAX << 1;
}

/**
 * Fillup an pollfd array with file descriptors to poll
 *
 * @param p: Common poller dupfd instance
 * @param pfd: Array to fill
 * @param nr: Size of array
 * @return: the number of fd on success, negative number otherwise
 */
static int dupfd_descriptors(struct poller *p, struct pollfd *pfd, size_t nr)
{
	struct dupfd *d = to_dupfd(p);
	size_t i;

	if(nr != (DUPFD_POLLFD_MAX << 1))
		return -EINVAL;

	for(i = 0; i < DUPFD_POLLFD_MAX; ++i) {
		pfd[(i << 1)].fd = d->infd[i];
		pfd[(i << 1)].events = POLLIN;
		pfd[(i << 1) + 1].fd = d->outfd[i];
		pfd[(i << 1) + 1].events = POLLOUT;
	}

	return DUPFD_POLLFD_MAX << 1;
}

/**
 * Demangle poll result events
 *
 * @param p: Common poller dupfd instance
 * @param pfd: Array of pollfd to demangle
 * @param nr: Size of array
 * @param revents: Demangled result poll events
 * @return: 0 on success, negative number otherwise
 */
static int dupfd_poll_revents(struct poller *p, struct pollfd *pfd, size_t nr,
		unsigned short *revents)
{
	struct pollfd sfd[DUPFD_POLLFD_MAX];
	size_t snr;
	int ret;
	(void)pfd;
	(void)nr;

	snr = snd_pcm_poll_descriptors_count(p->amx->slave);
	snd_pcm_poll_descriptors(p->amx->slave, sfd, snr);
	ret = poll(sfd, snr, 0);
	if(ret < 0) {
		AMUX_ERR("%s: poll() error\n", __func__);
		return -errno;
	}
	snd_pcm_poll_descriptors_revents(p->amx->slave, sfd, snr, revents);

	/* We woke up to soon, playback is not ready */
	if(snd_pcm_avail_update(p->amx->slave) <
			(snd_pcm_sframes_t)p->amx->io.period_size)
		*revents &= ~POLLOUT;

	return 0;
}

/**
 * Update dupfd current slave
 *
 * @param p: Common poller dupfd instance
 * @return: 0 on success, negative number otherwise
 */
static int dupfd_set_slave(struct poller *p)
{
	struct dupfd *d = to_dupfd(p);
	struct pollfd sfd[DUPFD_POLLFD_MAX];
	size_t snr, i;
	int ret;

	snr = snd_pcm_poll_descriptors_count(p->amx->slave);
	if(snr > (int)ARRAY_SIZE(sfd)) {
		AMUX_ERR("%s: Slave PCM has too many poll fd\n", __func__);
		return -EINVAL;
	}

	if(snd_pcm_poll_descriptors(p->amx->slave, sfd, snr) < 0) {
		AMUX_ERR("Can't get poll descriptor\n");
		return -1;
	}

	/*
	 * Fill the poll descriptor array, first with mock poll descriptors
	 * pointing to the slave ones. If slave PCM has less descriptor than
	 * the pfds array, the remaining spaces are fill with mock pipe
	 * descriptors.
	 */
	for(i = 0; i < DUPFD_POLLFD_MAX; ++i) {
		/* Read fd */
		if(sfd[i % snr].events & POLLIN)
			ret = dup2(sfd[i % snr].fd, d->infd[i]);
		else
			ret = dup2(d->efd[1], d->infd[i]);
		if(ret < 0) {
			AMUX_ERR("%s: cannot dup2\n", __func__);
			goto out;
		}

		/* Write fd */
		if(sfd[i % snr].events & POLLOUT)
			ret = dup2(sfd[i % snr].fd, d->outfd[i]);
		else
			ret = dup2(d->efd[0], d->outfd[i]);
		if(ret < 0) {
			AMUX_ERR("%s: cannot dup2\n", __func__);
			goto out;
		}
	}

	ret = 0;
out:
	return ret;
}

/**
 * Create a new dupfd poller instance.
 *
 * @param p: Resulting poller instance
 * @param args: dupfd arguments
 * @return: 0 on success, negative number otherwise.
 */
static int dupfd_create(struct poller **p, void *args)
{
	struct dupfd *d;
	(void)args;

	AMUX_DBG("%s: enter\n", __func__);

	d = malloc(sizeof(*d));
	if(d == NULL)
		return -ENOMEM;

	dupfd_init(d);
	*p = &d->p;

	return 0;
}

/**
 * Destroy a dupfd poller instance.
 *
 * @param p: dupfd poller instance to destroy
 */
static void dupfd_destroy(struct poller *p)
{
	struct dupfd *d = to_dupfd(p);

	AMUX_DBG("%s: enter\n", __func__);
	dupfd_cleanup(d);
	free(d);
}

static struct poller_ops const dupfd_ops = {
	.create = dupfd_create,
	.destroy = dupfd_destroy,
	.set_slave = dupfd_set_slave,
	.descriptors_count = dupfd_descriptors_count,
	.descriptors = dupfd_descriptors,
	.poll_revents = dupfd_poll_revents,
};

static struct poller_desc const dupfd_desc = {
	.name = "dupfd",
	.ops = &dupfd_ops,
};

POLLER_REGISTER(dupfd_desc);
