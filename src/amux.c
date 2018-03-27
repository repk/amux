#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <stddef.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <alsa/asoundlib.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

//#define AMUX_DUPFD

#ifndef AMUX_DUPFD
#include <pthread.h>
#include <sys/eventfd.h>
#endif

//#define DEBUG

#ifdef DEBUG
#define AMUX_DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define AMUX_DBG(...)
#endif

#define AMUX_ERR(...) fprintf(stderr, __VA_ARGS__)
#define AMUX_WARN(...) fprintf(stdout, __VA_ARGS__)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(s) (sizeof(s) / sizeof(*(s)))
#endif

#ifndef container_of
/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:        the pointer to the member.
 * @type:       the type of the container struct this is embedded in.
 * @member:     the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	((type *)(__mptr - offsetof(type, member))); })
#endif

#define CARD_STRSZ 32
#define SLAVENR 32
#define AMUX_POLLFD_MAX 8

/**
 * Amux master PCM structure
 */
struct snd_pcm_amux {
	/**
	 * IO plugin interface
	 */
	struct snd_pcm_ioplug io;
	/**
	 * Currently selected PCM slave
	 */
	snd_pcm_t *slave;
	/**
	 * List of multiplexed slaves
	 */
	char *sname[SLAVENR];
	/**
	 * Current open stream
	 */
	snd_pcm_stream_t stream;
	/**
	 * Configured ring buffer boundary
	 */
	snd_pcm_uframes_t boundary;
#ifndef AMUX_DUPFD
	/**
	 * File descriptor array to poll (one fd is reserved for thread
	 * communication).
	 */
	struct pollfd pfd[AMUX_POLLFD_MAX + 1];
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
#else
	/**
	 * Mock poll file descriptor array.
	 * This array is used to present a constant poll * interface to user.
	 * When slave changes, those file descriptors are set to point to the
	 * new slave's fds, but their file descriptor numbers do not change.
	 * Thus making the slave switch operation transparent for user.
	 */
	int pollfd[AMUX_POLLFD_MAX];
#endif
	/**
	 * Total number of selectable multiplexed slave
	 */
	size_t slavenr;
	/**
	 * Currently used slave index in sname array
	 */
	size_t idx;
	/**
	 * Current open mode
	 */
	int mode;
	/**
	 * Slave configuration file descriptor
	 */
	int fd;
};
/**
 * Convert an IO plugin interface pointer to Amux PCM structure.
 */
#define to_pcm_amux(p) (container_of(p, struct snd_pcm_amux, io))

#ifndef AMUX_DUPFD
/**
 * Wake up the polling thread.
 *
 * @params amx: Amux master PCM
 */
static inline void amux_poll_wake(struct snd_pcm_amux *amx)
{
	uint64_t discard = 1;
	write(amx->pfd[0].fd, &discard, sizeof(discard));
}

/**
 * Ack the wake up signal
 *
 * @params amx: Amux master PCM
 */
static inline void amux_poll_ack(struct snd_pcm_amux *amx)
{
	uint64_t discard;
	read(amx->pfd[0].fd, &discard, sizeof(discard));
	(void)discard;
}

/**
 * Unblock user if it tries to poll
 *
 * @params amx: Amux master PCM
 */
static inline void amux_user_unblock(struct snd_pcm_amux *amx)
{
	uint64_t discard = 1;
	write(amx->eventfd, &discard, sizeof(discard));
}

/**
 * Block user if it tries to poll
 *
 * @params amx: Amux master PCM
 */
static inline void amux_user_block(struct snd_pcm_amux *amx)
{
	uint64_t discard;
	read(amx->eventfd, &discard, sizeof(discard));
	(void)discard;
}
#endif

/**
 * Allocate and init a new amux PCM structure.
 *
 * @return: New amux PCM on success, NULL pointer if allocation failed.
 */
static inline struct snd_pcm_amux *amux_create(void)
{
	struct snd_pcm_amux *amx;

	amx = calloc(1, sizeof(*amx));
	if(amx == NULL)
		goto out;

	amx->fd = -1;
#ifdef AMUX_DUPFD
	do {
		size_t i;
		for(i = 0; i < AMUX_POLLFD_MAX; ++i)
			amx->pollfd[i] = -1;
	} while(0);
#else
	amx->eventfd = eventfd(1, EFD_CLOEXEC);
	if(amx->eventfd < 0) {
		AMUX_ERR("Cannot create eventfd\n");
		free(amx);
		amx = NULL;
		goto out;
	}
	amx->pfd[0].fd = eventfd(0, EFD_CLOEXEC);
	if(amx->pfd[0].fd < 0) {
		AMUX_ERR("Cannot create eventfd\n");
		free(amx);
		amx = NULL;
		goto out;
	}
	amx->pfd[0].events = POLLIN;
	amx->pfdnr = 1;
	amx->pfdidx = -1;
	pthread_mutex_init(&amx->lock, NULL);
#endif
out:
	return amx;
}

/**
 * Cleanup an Amux PCM
 *
 * @param amx: PCM to destroy
 */
static inline void amux_destroy(struct snd_pcm_amux *amx)
{
	size_t i;

	if(amx == NULL)
		return;

	if(amx->slave)
		snd_pcm_close(amx->slave);

#ifdef AMUX_DUPFD
	for(i = 0; i < AMUX_POLLFD_MAX; ++i) {
		if(amx->pollfd[i] != -1)
			close(amx->pollfd[i]);
	}
#else
	close(amx->eventfd);
	close(amx->pfd[0].fd);
	pthread_mutex_destroy(&amx->lock);
#endif

	for(i = 0; i < amx->slavenr; ++i)
		free(amx->sname[i]);

	if(amx->fd >= 0)
		close(amx->fd);

	free(amx);
}

/**
 * Check if the configured slave matches the currently used one.
 *
 * @param amx: Amux master PCM
 * @return: -1 if configured PCM is different from current one or an error
 * occured, 0 otherwise.
 */
static inline int amux_check_card(struct snd_pcm_amux *amx)
{
	ssize_t n;
	int ret = -1;
	char card;

	lseek(amx->fd, SEEK_SET, 0);

	n = read(amx->fd, &card, 1);
	if(n < 0)
		goto out;

	card -= '0';
	if((size_t)card == amx->idx) {
		ret = 0;
		goto out;
	}

out:
	return ret;
}

/**
 * Close callback of an IO plugin PCM device.
 *
 * @param io: The IO plugin to close.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_close(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);
#ifndef AMUX_DUPFD
	amx->stop = 1;
	amux_poll_wake(amx);
	pthread_join(amx->pollth, NULL);
#endif
	amux_destroy(amx);

	return 0;
}

/**
 * Callback for starting IO plugin PCM playback.
 *
 * @param io: The IO plugin interface to start.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_start(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return -EPIPE;

	snd_pcm_start(amx->slave);

	return 0;
}

/**
 * Stop callback of an IO plugin PCM. This drops current playback buffers.
 *
 * @param io: The IO plugin interface to stop.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_stop(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return -EPIPE;

	snd_pcm_drop(amx->slave);

	return 0;
}

/**
 * Prepare callback of IO plugin PCM.
 *
 * @param io: The IO plugin interface to prepare.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_prepare(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return 0;

#ifdef DEBUG
	do {
		snd_output_t *out;
		snd_output_stdio_attach(&out, stderr, 0);
		snd_pcm_dump(io->pcm, out);
		snd_output_close(out);
	} while(0);
#endif

	return snd_pcm_prepare(amx->slave);
}

/*
 * Callback to query IO plugin PCM's channel mapping.
 *
 * @param io: IO plugin interface to query channel mapping from.
 * @return: The channel mapping
 */
static snd_pcm_chmap_query_t **amux_query_chmaps(snd_pcm_ioplug_t *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	return snd_pcm_query_chmaps(amx->slave);
}

/*
 * Callback to configure IO plugin PCM's channel mapping.
 *
 * @param io: IO plugin interface to configure channel mapping.
 * @param map: The channel mapping
 */
static int amux_set_chmap(snd_pcm_ioplug_t *io, snd_pcm_chmap_t const *map)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	return snd_pcm_set_chmap(amx->slave, map);
}

/*
 * Callback to configure IO plugin PCM's software params.
 *
 * @param io: IO plugin interface to configure.
 * @param parm: Software params to configure with.
 */
static int amux_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *parm)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	int ret;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	ret = snd_pcm_sw_params(amx->slave, parm);
	if(ret < 0) {
		AMUX_ERR("%s: Cannot configure slave sw params\n", __func__);
		goto out;
	}

	ret = snd_pcm_sw_params_get_boundary(parm, &amx->boundary);
out:
	return ret;
}

/*
 * Configure PCM slave and refine amux master hardware params
 *
 * @param amx: Amux master.
 * @param hw: Hardware params to configure with.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_hw_params_refine(struct snd_pcm_amux *amx,
		snd_pcm_hw_params_t *hw)
{
	snd_pcm_t *mst = amx->io.pcm, *slv = amx->slave;
	snd_pcm_hw_params_t *shw, *nmhw;
	snd_pcm_access_t acc;
	snd_pcm_format_t fmt;
	snd_pcm_uframes_t bsz;
	unsigned int val;
	int dir, ret;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, &amx->io);

	snd_pcm_hw_params_alloca(&shw);
	snd_pcm_hw_params_alloca(&nmhw);

	snd_pcm_hw_params_any(slv, shw);
	snd_pcm_hw_params_any(mst, nmhw);

	/*
	 * XXX unfortunately we need to allow resampling (e.g mpv disables
	 * resampling but keep previous set sample rate value).
	 */
	ret = snd_pcm_hw_params_set_rate_resample(slv, shw, 1);
	if(ret != 0) {
		AMUX_ERR("Cannot set rate resample\n");
		goto out;
	}
	ret = snd_pcm_hw_params_set_rate_resample(mst, nmhw, 1);
	if(ret != 0) {
		AMUX_ERR("Cannot set rate resample\n");
		goto out;
	}

	/* Force slave's MMAP INTERLEAVED access */
	ret = snd_pcm_hw_params_set_access(slv, shw,
			SND_PCM_ACCESS_MMAP_INTERLEAVED);
	if(ret != 0) {
		AMUX_ERR("Cannot set access to MMAP_INTERLEAVED\n");
		goto out;
	}

	snd_pcm_hw_params_get_access(hw, &acc);
	ret = snd_pcm_hw_params_set_access(mst, nmhw, acc);
	if(ret != 0) {
		AMUX_ERR("Cannot set access to %d\n", acc);
		goto out;
	}

	snd_pcm_hw_params_get_format(hw, &fmt);
	ret = snd_pcm_hw_params_set_format(slv, shw, fmt);
	if(ret != 0) {
		AMUX_ERR("Cannot set fmt to %d\n", (int)fmt);
		goto out;
	}
	ret = snd_pcm_hw_params_set_format(mst, nmhw, fmt);
	if(ret != 0) {
		AMUX_ERR("Cannot set fmt to %d\n", (int)fmt);
		goto out;
	}

	snd_pcm_hw_params_get_channels(hw, &val);
	ret = snd_pcm_hw_params_set_channels(slv, shw, val);
	if(ret != 0) {
		AMUX_ERR("Cannot set channels to %u\n", val);
		goto out;
	}
	ret = snd_pcm_hw_params_set_channels(mst, nmhw, val);
	if(ret != 0) {
		AMUX_ERR("Cannot set channels to %u\n", val);
		goto out;
	}

	snd_pcm_hw_params_get_rate(hw, &val, &dir);
	ret = snd_pcm_hw_params_set_rate(slv, shw, val, dir);
	if(ret != 0) {
		AMUX_ERR("Cannot set precise rate %u (please use a plug)\n",
				val);
		goto out;
	}
	ret = snd_pcm_hw_params_set_rate(mst, nmhw, val, dir);
	if(ret != 0) {
		AMUX_ERR("Cannot set rate %u\n", val);
		goto out;
	}

	snd_pcm_hw_params_get_buffer_size(hw, &bsz);
	ret = snd_pcm_hw_params_set_buffer_size_near(slv, shw, &bsz);
	if(ret != 0) {
		AMUX_ERR("Cannot set buffer size to %u\n", (unsigned int)bsz);
		goto out;
	}
	ret = snd_pcm_hw_params_set_buffer_size(mst, nmhw, bsz);
	if(ret != 0) {
		AMUX_ERR("Cannot set buffer size to %u\n", (unsigned int)bsz);
		goto out;
	}

	snd_pcm_hw_params_get_period_size(hw, &bsz, &dir);
	ret = snd_pcm_hw_params_set_period_size_near(slv, shw, &bsz, &dir);
	if(ret != 0) {
		AMUX_ERR("Cannot set period size to %u\n", (unsigned int)bsz);
		goto out;
	}
	ret = snd_pcm_hw_params_set_period_size(mst, nmhw, bsz, dir);
	if(ret != 0) {
		AMUX_ERR("Cannot set period size to %u\n", (unsigned int)bsz);
		goto out;
	}

	ret = snd_pcm_hw_params(slv, shw);
	if(ret != 0) {
		AMUX_ERR("Cannot set slave's hw params\n");
		goto out;
	}

	snd_pcm_hw_params_copy(hw, nmhw);
out:
	return ret;
}

/**
 * Configure new slave PCM.
 *
 * @param amx: Amux master.
 * @param idx: New slave index.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_cfg_slave(struct snd_pcm_amux *amx, size_t idx)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	struct pollfd sfd[AMUX_POLLFD_MAX];
	int snr, ret;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, &amx->io);

	if(idx > amx->slavenr) {
		AMUX_ERR("%s: Index (%lu) invalid\n", __func__,
				(unsigned long)idx);
		return -EINVAL;
	}

	amx->idx = idx;
	snd_pcm_drop(amx->slave);
	snd_pcm_close(amx->slave);
	ret = snd_pcm_open(&amx->slave, amx->sname[idx], amx->stream,
			amx->mode);
	if(ret != 0) {
		AMUX_ERR("%s: snd_pcm_open error\n", __func__);
		goto out;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_current(amx->io.pcm, hw);
	ret = amux_hw_params_refine(amx, hw);
	if(ret != 0) {
		AMUX_ERR("%s: amux_hw_params_refine error\n", __func__);
		goto out;
	}

	/* TODO check hw period size */
#if 0
	snd_pcm_uframes_t val;
	int dir;
	snd_pcm_hw_params_get_period_size(hw, &val, &dir);
	if(amx->io.period_size != val) {
		AMUX_ERR("%s: Period size\n", __func__);
		*(uint32_t *)0x0 = 12;
	}
	amx->io.period_size = val;
	snd_pcm_hw_params_get_buffer_size(hw, &val);
	if(amx->io.buffer_size != val) {
		AMUX_ERR("%s: Buffer size\n", __func__);
		*(uint32_t *)0x0 = 12;
	}
	amx->io.buffer_size = val;
#endif

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(amx->io.pcm, sw);
	ret = snd_pcm_sw_params(amx->slave, sw);
	if(ret != 0) {
		AMUX_ERR("%s: snd_pcm_sw_params error\n", __func__);
		goto out;
	}

	/* TODO get/set chmaps */

	ret = snd_pcm_prepare(amx->slave);
	if(ret != 0) {
		AMUX_ERR("%s: snd_pcm_prepare error\n", __func__);
		goto out;
	}

	snr = snd_pcm_poll_descriptors_count(amx->slave);
	if(snr > (int)ARRAY_SIZE(sfd)) {
		AMUX_ERR("%s: Slave PCM has too many poll fd\n", __func__);
		return -EINVAL;
	}

	if(snd_pcm_poll_descriptors(amx->slave, sfd, snr) < 0) {
		AMUX_ERR("Can't get poll descriptor\n");
		return -1;
	}

#ifdef AMUX_DUPFD
	do {
		size_t i;
		for(i = 0; i < AMUX_POLLFD_MAX; ++i) {
			ret = dup2(sfd[i % snr].fd, amx->pollfd[i]);
			if(ret < 0) {
				AMUX_ERR("%s: cannot dup2\n", __func__);
				return ret;
			}
		}
	} while(0);
#else
	poll(sfd, snr, 0);
	pthread_mutex_lock(&amx->lock);
	if(snd_pcm_avail_update(amx->slave) <
			(snd_pcm_sframes_t)amx->io.period_size) {
		if(amx->pfdnr == 1)
			amux_user_block(amx);
		amx->pfdnr = snr + 1;
	} else {
		if(amx->pfdnr != 1)
			amux_user_unblock(amx);
		amx->pfdnr = 1;
	}
	amx->pfdidx = amx->idx;
	memcpy(amx->pfd + 1, sfd, snr * sizeof(*sfd));
	pthread_mutex_unlock(&amx->lock);
	amux_poll_wake(amx);
#endif
	ret = 0;
out:
	return ret;
}

/**
 * Switch slave PCM if configuration changed
 *
 * @param amx: Amux master
 * @return: 0 if slave hasn't changed or switch succeed, negative number
 * otherwise.
 */
static int amux_switch(struct snd_pcm_amux *amx)
{
	ssize_t n;
	int ret = -1;
	char card = '0' + amx->idx;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, &amx->io);

	lseek(amx->fd, SEEK_SET, 0);

	n = read(amx->fd, &card, 1);
	if(n < 0) {
		perror("Cannot read");
		goto out;
	}

	card -= '0';
	if((size_t)card == amx->idx) {
		ret = 0;
		goto out;
	}

	ret = amux_cfg_slave(amx, card);
out:
	return ret;
}

/**
 * Callback to configure IO plugin PCM hardware params.
 *
 * @param io: IO plugin interface.
 * @param params: Hardware param to configure plugin with.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_hw_params(struct snd_pcm_ioplug *io,
		snd_pcm_hw_params_t *params)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return -EPIPE;

	return amux_hw_params_refine(amx, params);
}

/**
 * Callback to get IO plugin's current playback/capture buffer hardware
 * position.
 *
 * @param io: IO plugin interface.
 * @return: The hardware buffer positiion in frame, can be negative on error.
 */
static snd_pcm_sframes_t amux_pointer(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	snd_pcm_sframes_t ret, avail;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_switch(amx) != 0)
		return -EPIPE;

	if(snd_pcm_state(amx->slave) != SND_PCM_STATE_RUNNING)
		snd_pcm_prepare(amx->slave);

	avail = snd_pcm_avail_update(amx->slave);
	if((snd_pcm_uframes_t)avail > io->buffer_size)
		avail = io->buffer_size;

	ret = avail + io->appl_ptr - io->buffer_size;
	if(ret < 0)
		ret += amx->boundary;
	else if((snd_pcm_uframes_t)ret >= amx->boundary)
		ret -= amx->boundary;

	return ret;
}

/**
 * Callback to get the number of poll file descriptor of an IO plugin
 *
 * @param io: The IO plugin interface
 * @return: Always AMUX_POLLFD_MAX
 */
static int amux_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
	int ret;
	(void)io;
	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

#ifdef AMUX_DUPFD
	ret = AMUX_POLLFD_MAX;
#else
	ret = 1;
#endif
	/*
	 * Amux always exhibit the same number of polling descriptor. So that
	 * PCM slave switch is tranparent for the user
	 */
	return ret;
}

/**
 * Callback to get the actual IO plugin poll descriptors.
 *
 * @param io: The IO plugin interface.
 * @param pfds: Filled Poll descriptor array.
 * @param nr: Poll descriptor array size.
 * @return: 0 on success, negative number otherwise.
 */
static int amux_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfds,
		unsigned int nr)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	snd_pcm_state_t state;
	struct pollfd sfd[AMUX_POLLFD_MAX];
	int snr;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_switch(amx) != 0) {
		AMUX_ERR("%s: PCM slave switching error\n", __func__);
		return -EPIPE;
	}

	/* TODO check nr == AMUX_POLLFD_MAX */

	state = snd_pcm_state(amx->slave);
	if(state == SND_PCM_STATE_XRUN ||
			state == SND_PCM_STATE_PREPARED) {
		/*
		 * Try to recover an xrun, some programs poll before PCM is
		 * actually started
		 */
		snd_pcm_prepare(amx->slave);
		snd_pcm_start(amx->slave);
	} else if (snd_pcm_state(amx->slave) != SND_PCM_STATE_RUNNING) {
		/* XXX This should not happen */
		AMUX_ERR("%s: Invalid PCM state %d\n", __func__,
				snd_pcm_state(amx->slave));
		return -EPIPE;
	}

	/* Fetch slave poll descriptors (in case of slave switch) */
	snr = snd_pcm_poll_descriptors_count(amx->slave);
	if(snr > (int)ARRAY_SIZE(sfd)) {
		AMUX_ERR("%s: Slave PCM has too many poll fd\n", __func__);
		return -EINVAL;
	}
	if(snd_pcm_poll_descriptors(amx->slave, sfd, snr) < 0) {
		AMUX_ERR("Can't get poll descriptor\n");
		return -1;
	}

#ifdef AMUX_DUPFD
	do {
		size_t i;
		int ret;
		/*
		 * Fill the poll descriptor array, first with mock poll
		 * descriptors pointing to the slave ones. If slave PCM has
		 * less descriptor than the pfds array, the remaining spaces
		 * are still fill with another mock poll descriptors pointing
		 * to slave ones.
		 */
		for(i = 0; i < AMUX_POLLFD_MAX; ++i) {
			/*
			 * On first time create the mock poll descriptors with
			 * dup(), then on next calls reuse the mock poll
			 * descriptors numbers with dup2().
			 */
			if(amx->pollfd[i] < 0)
				ret = dup(sfd[i % snr].fd);
			else
				ret = dup2(sfd[i % snr].fd, amx->pollfd[i]);

			if(ret < 0)
				return ret;

			amx->pollfd[i] = ret;
			pfds[i].fd = amx->pollfd[i];
			pfds[i].events = POLLIN | POLLOUT;
		}
	} while(0);
#else
	poll(sfd, snr, 0);
	pthread_mutex_lock(&amx->lock);
	if(snd_pcm_avail_update(amx->slave) <
			(snd_pcm_sframes_t)amx->io.period_size) {
		if(amx->pfdnr == 1)
			amux_user_block(amx);
		amx->pfdnr = snr + 1;
	} else {
		if(amx->pfdnr != 1)
			amux_user_unblock(amx);
		amx->pfdnr = 1;
	}
	amx->pfdidx = amx->idx;
	memcpy(amx->pfd + 1, sfd, snr * sizeof(*sfd));
	pthread_mutex_unlock(&amx->lock);

	pfds[0].fd = amx->eventfd;
	pfds[0].events = POLLIN;

	amux_poll_wake(amx);
#endif

	return nr;
}

/**
 * Callback to handle IO plugin poll descriptors events.
 *
 * @param io: IO plugin interface.
 * @param pfds: Poll descriptor array.
 * @param nfds: Poll descriptor array size.
 * @param revents: Resulting poll events
 * @return: 0 on success with revents filled up with appropriate events,
 * negative number otherwise.
 */
static int amux_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfds,
		unsigned int nfds, unsigned short *revents)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_switch(amx) != 0) {
		AMUX_ERR("%s: PCM slave switching error\n", __func__);
		return -EPIPE;
	}

#ifdef AMUX_DUPFD
	nfds = snd_pcm_poll_descriptors_count(amx->slave);
	snd_pcm_poll_descriptors_revents(amx->slave, pfds, nfds, revents);
	/* We woke up to soon, playback is not ready */
	if(snd_pcm_avail_update(amx->slave) <
			(snd_pcm_sframes_t)io->period_size)
		*revents &= ~POLLOUT;
#else
	*revents = 0;
	if((nfds != 1) || (pfds[0].fd != amx->eventfd))
		return 0;

	if(pfds[0].revents != POLLIN)
		return 0;

	pthread_mutex_lock(&amx->lock);
	if(amx->pfdidx != amx->idx) {
		pthread_mutex_unlock(&amx->lock);
		goto out;
	}

	snd_pcm_poll_descriptors_revents(amx->slave, amx->pfd + 1,
			snd_pcm_poll_descriptors_count(amx->slave), revents);

	if(snd_pcm_avail_update(amx->slave) <
			(snd_pcm_sframes_t)amx->io.period_size) {
		/* We woke up to soon, playback is not ready */
		if(amx->pfdnr == 1)
			amux_user_block(amx);
		amx->pfdnr = snd_pcm_poll_descriptors_count(amx->slave) + 1;
		amux_poll_wake(amx);
	}
	pthread_mutex_unlock(&amx->lock);
out:
#endif
	return 0;
}

/**
 * Callback for IO plugin transfer data.
 *
 * @param io: IO plugin interface.
 * @param areas: Channel frames
 * @param offset: offset of data in channel frames
 * @param size: size of data to transfer
 * @return: the number of transferred frames, can be negative on error.
 */
static snd_pcm_sframes_t amux_transfer(struct snd_pcm_ioplug *io,
		snd_pcm_channel_area_t const *areas,
		snd_pcm_uframes_t offset, snd_pcm_uframes_t size)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	snd_pcm_channel_area_t const *sareas;
	snd_pcm_uframes_t xfer = 0, soffset;
	snd_pcm_uframes_t ssize = size;
	snd_pcm_sframes_t ret, tmp;
	snd_pcm_state_t state;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_switch(amx) != 0)
		return -EPIPE;

	/* Check buffers integrity */
	tmp = snd_pcm_avail(amx->io.pcm);
	ret = snd_pcm_avail_update(amx->slave);
	if(ret < tmp) {
		AMUX_ERR("%s: Our buffer is not synchronized with the slave "
				"one, something bad happened\n", __func__);
		return -EPIPE;
	} else if(ret < (snd_pcm_sframes_t)size) {
		AMUX_ERR("%s: Write size is bigger than available "
				"buffer size (%lu/%lu)\n", __func__,
				(unsigned long)ret, (unsigned long)size);
		return -EPIPE;
	}

	while(size > xfer) {
		snd_pcm_mmap_begin(amx->slave, &sareas, &soffset, &ssize);
		snd_pcm_areas_copy(sareas, soffset, areas, offset,
				io->channels, ssize, io->format);
		ret = snd_pcm_mmap_commit(amx->slave, soffset, ssize);
		if(ret < 0)
			break;
		offset += ret;
		xfer += ret;
		ssize = size - xfer;
	}

	state = snd_pcm_state(amx->slave);
	/* Start slave if not started */
	if(state == SND_PCM_STATE_PREPARED)
		snd_pcm_start(amx->slave);
	else if(state != SND_PCM_STATE_RUNNING)
		return -EINVAL;

#ifndef AMUX_DUPFD
	if(snd_pcm_avail_update(amx->slave) <
			(snd_pcm_sframes_t)amx->io.period_size) {
		pthread_mutex_lock(&amx->lock);
		if(amx->pfdnr == 1)
			amux_user_block(amx);
		amx->pfdnr = snd_pcm_poll_descriptors_count(amx->slave) + 1;
		pthread_mutex_unlock(&amx->lock);
		amux_poll_wake(amx);
	}
#endif

	if(ret > 0)
		ret = xfer;

	return ret;
}

/**
 * Callback to dump IO plugin PCM param informations.
 *
 * @param io: IO plugin interface
 * @param out: Output interface to write into.
 */
static void amux_dump(snd_pcm_ioplug_t *io, snd_output_t *out)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	snd_output_printf(out, "%s\n", io->name);
	snd_output_printf(out, "Its setup is:\n");
	snd_pcm_dump_setup(io->pcm, out);
	snd_output_printf(out, "Slave: ");
	snd_pcm_dump(amx->slave, out);
}

/**
 * Amux IO plugin callbacks
 */
static struct snd_pcm_ioplug_callback amux_ops = {
	.close = amux_close,
	.start = amux_start,
	.stop = amux_stop,
	.hw_params = amux_hw_params,
	.sw_params = amux_sw_params,
	.set_chmap = amux_set_chmap,
	.query_chmaps = amux_query_chmaps,
	.prepare = amux_prepare,
	.pointer = amux_pointer,
	.transfer = amux_transfer,
	.poll_revents = amux_poll_revents,
	.poll_descriptors_count = amux_poll_descriptors_count,
	.poll_descriptors = amux_poll_descriptors,
	.dump = amux_dump,
};

#ifndef AMUX_DUPFD
/**
 * Thread that poll slave in background to detect when it is ready
 */
void *amux_poll_thread(void *arg)
{
	struct snd_pcm_amux *amx = (struct snd_pcm_amux *)arg;
	struct pollfd pfd[AMUX_POLLFD_MAX + 1];
	size_t nr, idx;
	int ret;

	while(!amx->stop) {
		pthread_mutex_lock(&amx->lock);
		nr = amx->pfdnr;
		idx = amx->pfdidx;
		memcpy(pfd, amx->pfd, nr * sizeof(*pfd));
		pthread_mutex_unlock(&amx->lock);

		ret = poll(pfd, nr, -1);
		if(ret < 0) {
			AMUX_ERR("Poll error\n");
			break;
		}
		if(pfd[0].revents != 0) {
			amux_poll_ack(amx);
			if(ret == 1)
				continue;
		}

		/* Fill poll result */
		pthread_mutex_lock(&amx->lock);
		/* Slave switched while poll returned */
		if(amx->pfdidx != idx) {
			pthread_mutex_unlock(&amx->lock);
			continue;
		}
		memcpy(amx->pfd, pfd, nr * sizeof(*pfd));
		amx->pfdnr = 1;
		pthread_mutex_unlock(&amx->lock);
		amux_user_unblock(amx);
	}

	return NULL;
}
#endif

/**
 * AMUX plugin's open function
 */
SND_PCM_PLUGIN_DEFINE_FUNC(amux) {
	struct snd_pcm_amux *amx;
	char const *pname = NULL, *fpath = NULL;
	snd_config_iterator_t i, next;
	int ret = -ENOMEM;
	char sidx;

	AMUX_DBG("%s: enter\n", __func__);

	amx = amux_create();
	if(amx == NULL)
		goto out;

	snd_config_for_each(i, next, conf) {
		snd_config_t *cfg = snd_config_iterator_entry(i);
		char const *id;
		if(snd_config_get_id(cfg, &id) < 0)
			continue;
		if(strcmp(id, "type") == 0)
			continue;
		if(strcmp(id, "comment") == 0)
			continue;
		if(strcmp(id, "hint") == 0)
			continue;
		if(strcmp(id, "file") == 0) {
			ret = snd_config_get_string(cfg, &fpath);
			if(ret < 0) {
				SNDERR("Invalid string for %s", id);
				goto out;
			}
			continue;
		}
		if(strcmp(id, "list") == 0) {
			snd_config_iterator_t _i, _next;
			snd_config_for_each(_i, _next, cfg) {
				snd_config_t *_cfg =
					snd_config_iterator_entry(_i);
				snd_config_t *pc;
				ret = snd_config_search(_cfg, "pcm", &pc);
				if(ret != 0) {
					SNDERR("Invalid slave for %s", name);
					goto out;
				}
				ret = snd_config_get_string(pc, &pname);
				if(ret < 0) {
					SNDERR("Invalid slave name for %s",
							name);
					goto out;
				}
				amx->sname[amx->slavenr++] = strdup(pname);
			}
			continue;
		}
		SNDERR("Unknown field %s", id);
		goto out;
	}

	ret = open(fpath, O_RDONLY);
	if(ret < 0)
		goto out;

	amx->fd = ret;
	ret = read(amx->fd, &sidx, 1);
	if(ret < 0)
		goto out;

	sidx -= '0';
	if((sidx < 0) || ((size_t)sidx > amx->slavenr)) {
		ret = -EINVAL;
		goto out;
	}

	amx->idx = sidx;
	ret = snd_pcm_open(&amx->slave, amx->sname[amx->idx], stream, mode);
	if(ret != 0)
		goto out;

	amx->io.version = SND_PCM_IOPLUG_VERSION;
	amx->io.name = "Amux live PCM card multiplexer plugin";
	amx->io.callback = &amux_ops;
	amx->stream = stream;
	amx->mode = mode;
	amx->io.poll_fd = -1;
	amx->io.poll_events = POLLOUT;
	ret = snd_pcm_ioplug_create(&amx->io, name, stream, mode);
	if(ret != 0)
		goto out;

#ifndef AMUX_DUPFD
	ret = pthread_create(&amx->pollth, NULL, amux_poll_thread,
			(void *)amx);
	if(ret != 0) {
		AMUX_ERR("Cannot create poll thread\n");
		snd_pcm_ioplug_delete(&amx->io);
		goto out;
	}
#endif

	*pcmp = amx->io.pcm;

	AMUX_DBG("Create new ioplug PCM %p\n", &amx->io);
out:
	if(ret != 0) {
		AMUX_ERR("Cannot Open PCM %d\n", ret);
		amux_destroy(amx);
	}
	return ret;
}

SND_PCM_PLUGIN_SYMBOL(amux);
