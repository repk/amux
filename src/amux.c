#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <stddef.h>
#include <errno.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "amux.h"
#include "poller/poller.h"

#define CARD_STRSZ 32
#define AMUX_POLLFD_MAX 4

/**
 * Check if libasound is old and flawed. Libraries before 1.1.4 need to setup hw
 * constraints and suffer from dealock.
 *
 * @return: 0 if libasound is recent enough, 1 if too old and negative number on
 * error
 */
static inline int amux_libasound_need_kludge(void)
{
	unsigned int maj, min, rev;
	int ret;

	ret = sscanf(snd_asoundlib_version(), "%u.%u.%u", &maj, &min, &rev);
	if(ret != 3) {
		AMUX_ERR("%s: Cannot parse libasound version\n", __func__);
		return -EINVAL;
	}

	if((maj > 1) || (min > 1) || (rev > 3))
		return 0;

	return 1;
}

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
	if(amux_libasound_need_kludge())
		amx->asound_kludge = 1;
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

	if(amx->poller)
		poller_destroy(amx->poller);

	if(amx->slave)
		snd_pcm_close(amx->slave);

	for(i = 0; i < amx->slavenr; ++i)
		free(amx->sname[i]);

	if(amx->fd >= 0)
		close(amx->fd);

	free(amx);
}

/**
 * Initialize Amux PCM poller
 *
 * @param amx: Amux PCM to intialize poller with
 * @param name: Poller name
 * @return: 0 on success, negative number otherwise
 */
static inline int amux_poller_init(struct snd_pcm_amux *amx, char const *name)
{
	amx->poller = poller_create(amx, name, NULL);
	if(amx->poller == NULL)
		return -ENOMEM;

	return 0;
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
	int ret;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return 0;

	ret = snd_pcm_prepare(amx->slave);
	if(ret != 0) {
		AMUX_ERR("Can't prepare slave\n");
		return ret;
	}

	if(poller_set_slave(amx->poller) < 0) {
		AMUX_ERR("Can't set new slave\n");
		return -EPIPE;
	}

#ifdef DEBUG
	do {
		snd_output_t *out;
		snd_output_stdio_attach(&out, stderr, 0);
		snd_pcm_dump(io->pcm, out);
		snd_output_close(out);
	} while(0);
#endif
	return 0;
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
	int ret;

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

	if(poller_set_slave(amx->poller) != 0) {
		AMUX_ERR("Can't set poller's new slave\n");
		return -1;
	}

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
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	int ret;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	ret = poller_descriptors_count(amx->poller);
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

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_switch(amx) != 0) {
		AMUX_ERR("%s: PCM slave switching error\n", __func__);
		return -EPIPE;
	}

	state = snd_pcm_state(amx->slave);
	if(state == SND_PCM_STATE_XRUN ||
			state == SND_PCM_STATE_PREPARED) {
		/*
		 * Try to recover an xrun, some programs poll before PCM is
		 * actually started
		 */
		snd_pcm_prepare(amx->slave);
		snd_pcm_start(amx->slave);
	} else if (state != SND_PCM_STATE_RUNNING) {
		/* XXX This should not happen */
		AMUX_ERR("%s: Invalid PCM state %d\n", __func__, state);
		return -EPIPE;
	}

	if(poller_descriptors(amx->poller, pfds, nr) < 0) {
		AMUX_ERR("Can't get poll descriptor for user\n");
		return -EPIPE;
	}

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

	if(poller_poll_revents(amx->poller, pfds, nfds, revents) != 0) {
		AMUX_ERR("%s: poller revents error\n", __func__);
		return -EPIPE;
	}

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
	if(amx->asound_kludge) {
		tmp = amx->io.appl_ptr - amx->io.hw_ptr;
		if(tmp < 0)
			tmp += amx->boundary;
		tmp -= amx->io.buffer_size;
	} else {
		tmp = snd_pcm_avail(amx->io.pcm);
	}
	ret = snd_pcm_avail_update(amx->slave);
	if(ret < tmp) {
		AMUX_ERR("%s: Our buffer is not synchronized with the slave "
				"one, something bad happened "
				"(slave %ld / master %ld)\n", __func__,
				(long)ret, (long)tmp);
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

	poller_transfer(amx->poller);

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
 * Set some hw params constraints needed by some libasound versions to work.
 *
 * @param amx: Amux master IO plugin
 * return: 0 on success, negative number otherwise
 */
static int amux_set_hw_constraints(struct snd_pcm_amux *amx)
{
	snd_pcm_access_mask_t *amsk;
	snd_pcm_format_mask_t *fmsk;
	size_t i, fnr, anr;
	unsigned int acc[SND_PCM_ACCESS_LAST + 1];
	unsigned int fmt[SND_PCM_FORMAT_LAST + 1];
	int ret;

	snd_pcm_access_mask_alloca(&amsk);
	snd_pcm_format_mask_alloca(&fmsk);

	snd_pcm_access_mask_any(amsk);
	snd_pcm_format_mask_any(fmsk);

	for(i = 0, anr = 0; i < ARRAY_SIZE(acc); ++i) {
		if(snd_pcm_access_mask_test(amsk, i)) {
			acc[anr] = i;
			++anr;
		}
	}

	for(i = 0, fnr = 0; i < ARRAY_SIZE(fmt); ++i) {
		if(snd_pcm_format_mask_test(fmsk, i)) {
			fmt[fnr] = i;
			++fnr;
		}
	}

	ret = snd_pcm_ioplug_set_param_list(&amx->io, SND_PCM_IOPLUG_HW_ACCESS,
			anr, acc);
	if (ret < 0)
		goto out;

	ret = snd_pcm_ioplug_set_param_list(&amx->io, SND_PCM_IOPLUG_HW_FORMAT,
			fnr, fmt);
out:
	return ret;
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

/**
 * AMUX plugin's open function
 */
SND_PCM_PLUGIN_DEFINE_FUNC(amux) {
	struct snd_pcm_amux *amx;
	char const *pname = NULL, *fpath = NULL;
	char const *poller_name = POLLER_DEFAULT;
	snd_config_iterator_t i, next;
	int ret = -ENOMEM;
	char sidx;

	(void)root;

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
		if(strcmp(id, "poller") == 0) {
			ret = snd_config_get_string(cfg, &pname);
			if(ret < 0) {
				SNDERR("Invalid poller name");
				goto out;
			}
			poller_name = pname;
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
		ret = -EINVAL;
		goto out;
	}

	if(fpath == NULL) {
		ret = -EINVAL;
		AMUX_ERR("Missing mandatory file path in amux PCM config\n");
		goto out;
	}

	ret = amux_poller_init(amx, poller_name);
	if(ret < 0)
		goto out;

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

	/* Prior 1.1.4 asound libraries need to set minimal hw constraints */
	if(amx->asound_kludge)
		amux_set_hw_constraints(amx);

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
