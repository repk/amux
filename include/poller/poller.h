#ifndef _POLLER_H_
#define _POLLER_H_

struct poller;

/**
 * Poller operations
 */
struct poller_ops {
	/**
	 * Create a new poller instance
	 */
	int (*create)(struct poller **p, void *args);
	/**
	 * Destroy a poller instance
	 */
	void (*destroy)(struct poller *p);
	/**
	 * Update poller current slave
	 */
	int (*set_slave)(struct poller *p);
	/**
	 * Get number of poller's poll file descriptors
	 */
	int (*descriptors_count)(struct poller *p);
	/**
	 * Get actual poller's poll file descriptors
	 */
	int (*descriptors)(struct poller *p, struct pollfd *pfd, size_t nr);
	/**
	 * Demangle poll result events
	 */
	int (*poll_revents)(struct poller *p, struct pollfd *pfd, size_t nr,
			unsigned short *revents);
	/**
	 * Notify that a slave data transfer has been done
	 */
	void (*transfer)(struct poller *p);
};

/**
 * Description of poller implementation
 */
struct poller_desc {
	/**
	 * Poller identification name
	 */
	char *name;
	/**
	 * Poller specific operations
	 */
	struct poller_ops const *ops;
};

/**
 * Poller common structure.
 * Each poller implementation should include this.
 */
struct poller {
	/**
	 * Poller description, with poller specific operations
	 */
	struct poller_desc const *desc;
	/**
	 * Amux PCM master
	 */
	struct snd_pcm_amux *amx;
};

/**
 * Register a poller implementation
 */
#define POLLER_REGISTER(p) MODULE_REGISTER(poller, p)

struct poller *poller_create(struct snd_pcm_amux *amx, char const *name,
		void *args);
void poller_destroy(struct poller *p);
int poller_set_slave(struct poller *p);
int poller_descriptors_count(struct poller *p);
int poller_descriptors(struct poller *p, struct pollfd *pfd, size_t nr);
int poller_poll_revents(struct poller *p, struct pollfd *pfd, size_t nr,
		unsigned short *revents);
void poller_transfer(struct poller *p);

#endif
