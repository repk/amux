#include <stdlib.h>
#include <errno.h>
#include <sys/epoll.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "amux.h"
#include "poller/poller.h"

/**
 * Epoll based poller structure
 */
struct epoller {
	/**
	 * poller common structure
	 */
	struct poller p;
	/**
	 * Epoll interface file descriptor
	 */
	int epoll_fd;
	/**
	 * List of slave pollable file desc
	 */
	struct pollfd *sfd;
	/**
	 * Number of slave pollable file desc
	 */
	size_t snr;
};
#define to_epoller(poller) (container_of(poller, struct epoller, p))

/**
 * Initialize an epoller structure
 *
 * @param e: epoller to initialize
 * @return: 0 on success, negative number otherwise
 */
static inline int epoller_init(struct epoller *e)
{
	int ret;

	ret = epoll_create(1);
	if(ret < 0)
		return -errno;

	e->epoll_fd = ret;
	e->sfd =  NULL;
	e->snr = 0;

	return 0;
}

/**
 * Cleanup an epoller structure
 *
 * @param d: epoller to cleanup
 */
static inline void epoller_cleanup(struct epoller *e)
{
	close(e->epoll_fd);
	if(e->sfd)
		free(e->sfd);
}

/**
 * Return the number of file descriptor to poll.
 *
 * @param p: Common poller for epoller instance
 * @return: the number of file descriptors (i.e. always 1)
 */
static int epoller_descriptors_count(struct poller *p)
{
	(void)p;
	return 1;
}

/**
 * Fillup a pollfd array with file descriptors to poll
 *
 * @param p: Common poller for epoller instance
 * @param pfd: Array to fill
 * @param nr: Size of array
 * @return: the number of fd on success, negative number otherwise
 */
static int epoller_descriptors(struct poller *p, struct pollfd *pfd,
		size_t nr)
{
	struct epoller *e = to_epoller(p);

	if(nr != 1)
		return -EINVAL;

	pfd[0].fd = e->epoll_fd;
	pfd[0].events = POLLIN;

	return 1;
}

/**
 * Fetch actual poll result events
 *
 * @param p: Common poller for epoller instance
 * @param pfd: Array of pollfd to get event from
 * @param nr: Size of array
 * @param revents: Actual poll result
 * @return: 0 on success, negative number otherwise
 */
static int epoller_poll_revents(struct poller *p, struct pollfd *pfd,
		size_t nr, unsigned short *revents)
{
	struct epoller *e = to_epoller(p);
	snd_pcm_sframes_t avail;
	int ret;
	(void)pfd;
	(void)nr;

	ret = poll(e->sfd, e->snr, 0);
	if(ret < 0) {
		AMUX_ERR("%s: poll() error\n", __func__);
		return -errno;
	}
	snd_pcm_poll_descriptors_revents(p->amx->slave, e->sfd, e->snr,
			revents);

	avail = (snd_pcm_sframes_t)snd_pcm_avail_update(p->amx->slave);
	if(avail < 0)
		return avail;

	/* We woke up to soon, playback is not ready */
	if((avail < (snd_pcm_sframes_t)p->amx->io.period_size))
		*revents &= ~POLLOUT;

	return 0;
}

/**
 * Update epoller current slave
 *
 * @param p: Common poller for epoller instance
 * @return: 0 on success, negative number otherwise
 */
static int epoller_set_slave(struct poller *p)
{
	struct epoller *e = to_epoller(p);
	struct pollfd *sfd;
	size_t snr, i = 0;
	struct epoll_event ev;
	int ret;

	ret = -ENOMEM;
	snr = snd_pcm_poll_descriptors_count(p->amx->slave);
	sfd = malloc(sizeof(*sfd) * snr);
	if(sfd == NULL) {
		AMUX_ERR("%s: No memory\n", __func__);
		goto err;
	}

	ret = snd_pcm_poll_descriptors(p->amx->slave, sfd, snr);
	if(ret < 0) {
		AMUX_ERR("Can't get poll descriptor\n");
		goto err;
	}

	ev.data.ptr = NULL;
	for(i = 0; i < snr; ++i) {
		ev.events = 0;
		if(sfd[i].events & POLLOUT)
			ev.events |= EPOLLOUT;
		if(sfd[i].events & POLLIN)
			ev.events |= EPOLLIN;
		ret = epoll_ctl(e->epoll_fd, EPOLL_CTL_ADD, sfd[i].fd, &ev);
		if(ret != 0) {
			ret = -errno;
			goto err;
		}
	}

	for(i = 0; i < e->snr; ++i)
		epoll_ctl(e->epoll_fd, EPOLL_CTL_DEL, e->sfd[i].fd, NULL);
	if(e->sfd != NULL)
		free(e->sfd);

	e->sfd = sfd;
	e->snr = snr;
	return 0;
err:
	for(; i != 0; --i)
		epoll_ctl(e->epoll_fd, EPOLL_CTL_DEL, sfd[i - 1].fd, NULL);
	free(sfd);
	return ret;
}

/**
 * Create a new epoller instance
 *
 * @param p: Created common poller instance
 * @params args: epoller arguments
 * @return: 0 on success, negative number otherwise
 */
static int epoller_create(struct poller **p, void *args)
{
	struct epoller *e;
	(void)args;

	AMUX_DBG("%s: enter\n", __func__);

	e = malloc(sizeof(*e));
	if(e == NULL)
		return -ENOMEM;

	epoller_init(e);
	*p = &e->p;
	return 0;
}

/**
 * Destroy a epoller poller instance.
 *
 * @param p: common poller instance to destroy
 */
static void epoller_destroy(struct poller *p)
{
	struct epoller *e = to_epoller(p);

	AMUX_DBG("%s: enter\n", __func__);
	epoller_cleanup(e);
	free(e);
}

static struct poller_ops const epoller_ops = {
	.create = epoller_create,
	.destroy = epoller_destroy,
	.set_slave = epoller_set_slave,
	.descriptors_count = epoller_descriptors_count,
	.descriptors = epoller_descriptors,
	.poll_revents = epoller_poll_revents,
};

static struct poller_desc const epoller_desc = {
	.name = "epoller",
	.ops = &epoller_ops,
};

POLLER_REGISTER(epoller_desc);
