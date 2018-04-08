#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include <sys/eventfd.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "amux.h"
#include "poller/poller.h"

#define POLLTHR_POLLFD_MAX 16 /* Alsa lib max poll fd */

/**
 * thread poller structure
 */
struct pollthr {
	/**
	 * poller common structure
	 */
	struct poller p;
	/**
	 * File descriptor array to poll (one fd is reserved for thread
	 * communication).
	 */
	struct pollfd pfd[POLLTHR_POLLFD_MAX + 1];
	/**
	 * Number of file descritor in pfd array
	 */
	size_t pfdnr;
	/**
	 * Index of slave associated with the current poll array
	 */
	size_t pfdidx;
	/**
	 * Lock for poll fd array
	 */
	pthread_mutex_t lock;
	/**
	 * Polling thread handle
	 */
	pthread_t pollth;
	/**
	 * Event file used for alsa lib user polling
	 */
	int eventfd;
	/**
	 * Stop the polling thread
	 */
	uint8_t stop;
};
#define to_pollthr(poller) (container_of(poller, struct pollthr, p))

/**
 * Wake up the polling thread.
 *
 * @param pth: thread poller instance
 */
static inline void pollthr_wake(struct pollthr *pth)
{
	uint64_t discard = 1;
	write(pth->pfd[0].fd, &discard, sizeof(discard));
}

/**
 * Ack the wake up signal
 *
 * @param pth: thread poller instance
 */
static inline void pollthr_ack(struct pollthr *pth)
{
	uint64_t discard;
	ssize_t ret;
	ret = read(pth->pfd[0].fd, &discard, sizeof(discard));
	if(ret != sizeof(discard))
		AMUX_ERR("%s: cannot wake up poller thread\n", __func__);
	(void)discard;
}

/**
 * Unblock user if it tries to poll
 *
 * @param pth: thread poller instance
 */
static inline void pollthr_user_unblock(struct pollthr *pth)
{
	uint64_t discard = 1;
	write(pth->eventfd, &discard, sizeof(discard));
}

/**
 * Block user if it tries to poll
 *
 * @param pth: thread poller instance
 */
static inline void pollthr_user_block(struct pollthr *pth)
{
	uint64_t discard;
	ssize_t ret;
	ret = read(pth->eventfd, &discard, sizeof(discard));
	if(ret != sizeof(discard))
		AMUX_ERR("%s: cannot wake up poller thread\n", __func__);
	(void)discard;
}

/**
 * Initialize a pollthr structure
 *
 * @param pth: pollthr to initialize
 * @return: 0 on success, negative number otherwise
 */
static inline int pollthr_init(struct pollthr *pth)
{
	int ret;

	AMUX_DBG("%s: enter\n", __func__);

	ret = eventfd(1, EFD_CLOEXEC);
	if(ret < 0) {
		AMUX_ERR("%s: Cannot create eventfd\n", __func__);
		goto out;
	}
	pth->eventfd = ret;

	ret = eventfd(0, EFD_CLOEXEC);
	if(ret < 0) {
		AMUX_ERR("%s: Cannot create eventfd\n", __func__);
		close(pth->eventfd);
		goto out;
	}
	pth->pfd[0].fd = ret;
	pth->pfd[0].events = POLLIN;

	pth->pfdnr = 1;
	pth->pfdidx = -1;
	pth->stop = 0;
	pthread_mutex_init(&pth->lock, NULL);

	ret = 0;
out:
	return ret;
}

/**
 * Cleanup a pollthread structure
 *
 * @param pth: pollthread instance to clean
 */
static inline void pollthr_cleanup(struct pollthr *pth)
{
	AMUX_DBG("%s: enter\n", __func__);
	close(pth->eventfd);
	close(pth->pfd[0].fd);
	pthread_mutex_destroy(&pth->lock);
}

/**
 * Return the number of file descriptors to poll.
 *
 * @param p: Common poller poll thread instance
 * @return: the number of file descriptors
 */
static int pollthr_descriptors_count(struct poller *p)
{
	(void)p;
	AMUX_DBG("%s: enter\n", __func__);
	return 1;
}

/**
 * Fillup an pollfd array with file descriptors to poll
 *
 * @param p: Common poller poll thread instance
 * @param pfd: Array to fill
 * @param nr: Size of array
 * @return: the number of fd on success, negative number otherwise
 */
static int pollthr_descriptors(struct poller *p, struct pollfd *pfd, size_t nr)
{
	struct pollthr *pth = to_pollthr(p);

	AMUX_DBG("%s: enter\n", __func__);
	if(nr != 1) {
		AMUX_ERR("%s: Wrong number of file descriptors\n", __func__);
		return -EINVAL;
	}

	pfd[0].fd = pth->eventfd;
	pfd[0].events = POLLIN;

	return 1;
}

/**
 * Demangle poll result events
 *
 * @param p: Common poller poll thread instance
 * @param pfd: Array of pollfd to demangle
 * @param nr: Size of array
 * @param revents: Demangled result poll events
 * @return: 0 on success, negative number otherwise
 */
static int pollthr_poll_revents(struct poller *p, struct pollfd *pfd,
		size_t nr, unsigned short *revents)
{
	struct pollthr *pth = to_pollthr(p);
	AMUX_DBG("%s: enter\n", __func__);

	*revents = 0;
	if((nr != 1) || (pfd[0].fd != pth->eventfd))
		goto out;

	if(pfd[0].revents != POLLIN)
		goto out;

	pthread_mutex_lock(&pth->lock);
	if(pth->pfdidx != p->amx->idx) {
		pthread_mutex_unlock(&pth->lock);
		goto out;
	}

	snd_pcm_poll_descriptors_revents(p->amx->slave, pth->pfd + 1,
			snd_pcm_poll_descriptors_count(p->amx->slave),
			revents);

	if(snd_pcm_avail_update(p->amx->slave) <
			(snd_pcm_sframes_t)p->amx->io.period_size) {
		/* We woke up to soon, playback is not ready */
		if(pth->pfdnr == 1)
			pollthr_user_block(pth);
		pth->pfdnr = snd_pcm_poll_descriptors_count(p->amx->slave) + 1;
		pollthr_wake(pth);
		*revents &= ~POLLOUT;
	}
	pthread_mutex_unlock(&pth->lock);

out:
	return 0;
}

/**
 * Update poll thread current slave
 *
 * @param p: Common poller pollthr instance
 * @return: 0 on success, negative number otherwise
 */
static int pollthr_set_slave(struct poller *p)
{
	struct pollthr *pth = to_pollthr(p);
	struct pollfd sfd[POLLTHR_POLLFD_MAX];
	size_t snr;
	int ret;

	AMUX_DBG("%s: enter\n", __func__);

	snr = snd_pcm_poll_descriptors_count(p->amx->slave);
	if(snr > (int)ARRAY_SIZE(sfd)) {
		AMUX_ERR("%s: Slave PCM has too many poll fd\n", __func__);
		return -EINVAL;
	}

	if(snd_pcm_poll_descriptors(p->amx->slave, sfd, snr) < 0) {
		AMUX_ERR("Can't get poll descriptor\n");
		return -1;
	}

	ret = poll(sfd, snr, 0);
	if(ret < 0) {
		AMUX_ERR("%s: poll() error\n", __func__);
		return -errno;
	}
	pthread_mutex_lock(&pth->lock);
	if(snd_pcm_avail_update(p->amx->slave) <
			(snd_pcm_sframes_t)p->amx->io.period_size) {
		if(pth->pfdnr == 1)
			pollthr_user_block(pth);
		pth->pfdnr = snr + 1;
	} else {
		if(pth->pfdnr != 1)
			pollthr_user_unblock(pth);
		pth->pfdnr = 1;
	}
	pth->pfdidx = p->amx->idx;
	memcpy(pth->pfd + 1, sfd, snr * sizeof(*sfd));
	pthread_mutex_unlock(&pth->lock);
	pollthr_wake(pth);
	return 0;
}

/**
 * A new data transfer has been done
 *
 * @param p: Common poller poll thread instance
 */
static void pollthr_transfer(struct poller *p)
{
	struct pollthr *pth = to_pollthr(p);

	AMUX_DBG("%s: enter\n", __func__);

	if(snd_pcm_avail_update(p->amx->slave) <
			(snd_pcm_sframes_t)p->amx->io.period_size) {
		pthread_mutex_lock(&pth->lock);
		if(pth->pfdnr == 1)
			pollthr_user_block(pth);
		pth->pfdnr = snd_pcm_poll_descriptors_count(p->amx->slave) + 1;
		pthread_mutex_unlock(&pth->lock);
		pollthr_wake(pth);
	}
}


/**
 * Thread that poll slave in background to detect when it is ready
 */
static void *pollthr_thread(void *arg)
{
	struct pollthr *pth = (struct pollthr *)arg;
	struct pollfd pfd[POLLTHR_POLLFD_MAX + 1];
	size_t nr, idx;
	int ret;

	while(!pth->stop) {
		pthread_mutex_lock(&pth->lock);
		nr = pth->pfdnr;
		idx = pth->pfdidx;
		memcpy(pfd, pth->pfd, nr * sizeof(*pfd));
		pthread_mutex_unlock(&pth->lock);

		ret = poll(pfd, nr, -1);
		if(ret < 0) {
			AMUX_ERR("Poll error\n");
			break;
		}
		if(pfd[0].revents != 0) {
			pollthr_ack(pth);
			if(ret == 1)
				continue;
		}

		/* Fill poll result */
		pthread_mutex_lock(&pth->lock);
		/* Slave switched while poll returned */
		if(pth->pfdidx != idx) {
			pthread_mutex_unlock(&pth->lock);
			continue;
		}
		memcpy(pth->pfd, pfd, nr * sizeof(*pfd));
		pth->pfdnr = 1;
		pollthr_user_unblock(pth);
		pthread_mutex_unlock(&pth->lock);
	}

	return NULL;
}

/**
 * Create a new poll thread poller instance.
 *
 * @param p: Resulting poller instance
 * @param args: poll thread arguments
 * @return: 0 on success, negative number otherwise.
 */
static int pollthr_create(struct poller **p, void *args)
{
	struct pollthr *pth;
	int ret;
	(void)args;

	AMUX_DBG("%s: enter\n", __func__);

	pth = malloc(sizeof(*pth));
	if(pth == NULL)
		return -ENOMEM;

	ret = pollthr_init(pth);
	if(ret != 0) {
		AMUX_ERR("%s: Cannot init pollthr\n", __func__);
		free(pth);
		return ret;
	}

	ret = pthread_create(&pth->pollth, NULL, pollthr_thread, (void *)pth);
	if(ret != 0) {
		AMUX_ERR("%s: Cannot create poll thread\n", __func__);
		free(pth);
		return ret;
	}

	*p = &pth->p;

	return 0;
}

/**
 * Destroy a poll thread poller instance.
 *
 * @param p: poll thread poller instance to destroy
 */
static void pollthr_destroy(struct poller *p)
{
	struct pollthr *pth = to_pollthr(p);

	AMUX_DBG("%s: enter\n", __func__);
	pth->stop = 1;
	pollthr_wake(pth);
	pthread_join(pth->pollth, NULL);
	pollthr_cleanup(pth);
	free(pth);
}

static struct poller_ops const pollthr_ops = {
	.create = pollthr_create,
	.destroy = pollthr_destroy,
	.set_slave = pollthr_set_slave,
	.descriptors_count = pollthr_descriptors_count,
	.descriptors = pollthr_descriptors,
	.poll_revents = pollthr_poll_revents,
	.transfer = pollthr_transfer,
};

static struct poller_desc const pollthr_desc = {
	.name = "thread",
	.ops = &pollthr_ops,
};

POLLER_REGISTER(pollthr_desc);
