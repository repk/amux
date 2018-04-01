#ifndef _AMUX_H_
#define _AMUX_H_

#include <stdio.h>
#include <stddef.h>
#include <assert.h>

//#define DEBUG

#ifdef DEBUG
#define AMUX_DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define AMUX_DBG(...)
#endif

#define AMUX_ERR(...) fprintf(stderr, __VA_ARGS__)
#define AMUX_WARN(...) fprintf(stdout, __VA_ARGS__)

#ifdef DEBUG
#define AMUX_ASSERT(expr) assert(expr)
#else
#define AMUX_ASSERT(expr)
#endif

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

/* This is a workaround to gcc read only section attribute bug */
#define __SECTION_RDONLY ",\"a\"\n#"
#define MODULE_REGISTER(type, name)					\
	__attribute__((section(".rodata." # type __SECTION_RDONLY),	\
				used))					\
	static void const * const __ ## type ## _ ## name =		\
			(void const * const)&name

#define SLAVENR 32

struct poller;

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
	 * Poller instance, used to interface different way of poll slave
	 */
	struct poller *poller;
	/**
	 * Current open stream
	 */
	snd_pcm_stream_t stream;
	/**
	 * Configured ring buffer boundary
	 */
	snd_pcm_uframes_t boundary;
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

#define POLLER_DEFAULT "dupfd"
#endif
