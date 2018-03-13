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

#define AMUX_POLL

#ifndef AMUX_POLL
#include <unistd.h> /* TODO remove when usleep is no more needed */
#endif

//#define DEBUG

#ifdef DEBUG
#define AMUX_DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define AMUX_DBG(...)
#endif

#define AMUX_ERR(...) fprintf(stderr, __VA_ARGS__)

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

struct snd_pcm_amux {
	struct snd_pcm_ioplug io;
	snd_pcm_t *slave;
	char *sname[SLAVENR]; /* Slave PCM names */
	snd_pcm_stream_t stream;
	snd_pcm_uframes_t boundary;
	size_t slavenr;
	size_t idx; /* Selected slave idx */
	int mode;
	int fd;
};
#define to_pcm_amux(p) (container_of(p, struct snd_pcm_amux, io))

static inline struct snd_pcm_amux *amux_create(void)
{
	struct snd_pcm_amux *amx;

	amx = calloc(1, sizeof(*amx));
	if(amx == NULL)
		goto out;

	amx->fd = -1;
out:
	return amx;
}

static inline void amux_destroy(struct snd_pcm_amux *amx)
{
	size_t i;

	if(amx == NULL)
		return;

	if(amx->slave)
		snd_pcm_close(amx->slave);

	for(i = 0; i < amx->slavenr; ++i)
		free(amx->sname[i]);

	if(amx->fd >= 0)
		close(amx->fd);

	free(amx);
}

/* Check if current and configured PCM are still the same */
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
 * Close an IO plugin PCM device
 */
static int amux_close(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);
	amux_destroy(amx);

	return 0;
}

/**
 * Start PCM playback
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
 * Stop PCM playback
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
 * Prepare PCM playback
 */
static int amux_prepare(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return -EPIPE;

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
 * Get slave's PCM sw params
 */
static snd_pcm_chmap_query_t **amux_query_chmaps(snd_pcm_ioplug_t *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	return snd_pcm_query_chmaps(amx->slave);
}

/*
 * Configure slave's PCM channel map
 */
static int amux_set_chmap(snd_pcm_ioplug_t *io, snd_pcm_chmap_t const *map)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	return snd_pcm_set_chmap(amx->slave, map);
}

/*
 * Configure slave's PCM sw params
 */
static int amux_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *parm)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	int ret;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	ret = snd_pcm_sw_params(amx->slave, parm);
	snd_pcm_sw_params_get_boundary(parm, &amx->boundary);
	return ret;
}

/*
 * Configure PCM slave and refine master hwparams
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
 * Set hw params PCM
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
 * Get current DMA position
 */
static snd_pcm_sframes_t amux_pointer(struct snd_pcm_ioplug *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	snd_pcm_sframes_t ret, avail;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	avail = snd_pcm_avail_update(amx->slave);

	if(snd_pcm_state(amx->slave) != SND_PCM_STATE_RUNNING)
		return io->appl_ptr;

	ret = avail + io->appl_ptr - io->buffer_size;
	if(ret < 0)
		ret += amx->boundary;
	else if((snd_pcm_uframes_t)ret >= amx->boundary)
		ret -= amx->boundary;

	return ret;
}

#ifdef AMUX_POLL
static int amux_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return -EPIPE;

	return snd_pcm_poll_descriptors_count(amx->slave);
}

static int amux_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfds,
		unsigned int nr)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);
	snd_pcm_state_t state;

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return -EPIPE;

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

	if(snd_pcm_poll_descriptors(amx->slave, pfds, nr) < 0) {
		AMUX_ERR("Can't get poll descriptor\n");
		return -1;
	}

	return nr;
}
#endif

static int amux_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfds,
		unsigned int nfds, unsigned short *revents)
{
	struct snd_pcm_amux *amx = to_pcm_amux(io);

	AMUX_DBG("%s: enter PCM(%p)\n", __func__, io);

	if(amux_check_card(amx) != 0)
		return -EPIPE;

#ifdef AMUX_POLL
	snd_pcm_poll_descriptors_revents(amx->slave, pfds, nfds, revents);
	if(snd_pcm_avail_update(amx->slave) <
			(snd_pcm_sframes_t)io->period_size) {
		*revents &= ~POLLOUT;
		*revents |= POLLERR;
	}
#else
	while(snd_pcm_avail_update(amx->slave) <
		(snd_pcm_sframes_t)io->period_size) {
		usleep(10000);
	}

	*revents = POLLOUT;
#endif
	return 0;
}

/**
 * Transfer data to selected soundcard
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

	if(amux_check_card(amx) != 0)
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
	if(state == SND_PCM_STATE_PREPARED)
		snd_pcm_start(amx->slave);
	else if(state != SND_PCM_STATE_RUNNING)
		return -EINVAL;

	if(ret > 0)
		ret = xfer;

	return ret;
}

/**
 * Dump PCM params
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
#ifdef AMUX_POLL
	.poll_descriptors_count = amux_poll_descriptors_count,
	.poll_descriptors = amux_poll_descriptors,
#endif
	.dump = amux_dump,
};

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
#ifndef AMUX_POLL
	/* Open an always write ready fd */
	int fd[2];
	pipe(fd);
	amx->io.poll_fd = fd[1];
#else
	amx->io.poll_fd = -1;
#endif
	amx->io.poll_events = POLLOUT;
	ret = snd_pcm_ioplug_create(&amx->io, name, stream, mode);
	if(ret != 0)
		goto out;

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
