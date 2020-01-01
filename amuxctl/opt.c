#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>

#include "opt.h"

#define _PROGNAME_DFT "amuxctl"
#define PROGNAME(argc, argv) (((argc) > 0) ? (argv)[0] : _PROGNAME_DFT)
#define USAGE(argc, argv) usage(PROGNAME(argc, argv))

#define AM_SOPT_INIT(ao) do						\
{									\
	(ao)->pcm = NULL;						\
} while(0)

#define AM_SOPT_VALID(ao)						\
	(((ao)->act == AA_SET) && ((ao)->sopt.pcm != NULL))

#define AM_OPT_INIT(ao) do						\
{									\
	(ao)->act = AA_INVAL;						\
} while(0)

#define AM_OPT_VALID(ao)						\
	(((ao)->act == AA_LIST) || ((ao)->act == AA_GET) ||		\
	 (AM_SOPT_VALID(ao)))

static void usage(char const *progname)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s [OPTION]\n", progname);
	fprintf(stderr, "\t-l, --list\n");
	fprintf(stderr, "\t\tlist available PCM name\n");
	fprintf(stderr, "\t-s, --set <PCM>\n");
	fprintf(stderr, "\t\tconfigure PCM as system soundcard\n");
	fprintf(stderr, "\t-g, --get\n");
	fprintf(stderr, "\t\tget current system soundcard\n");
}

int parse_args(struct am_opt *aopt, int argc, char *argv[])
{
	struct option opt[] = {
		{
			.name = "list",
			.has_arg = 0,
			.flag = NULL,
			.val = 'l',
		},
		{
			.name = "set",
			.has_arg = 1,
			.flag = NULL,
			.val = 's',
		},
		{
			.name = "get",
			.has_arg = 0,
			.flag = NULL,
			.val = 'g',
		},
	};
	int idx, ret;

	AM_OPT_INIT(aopt);

	while((ret = getopt_long(argc, argv, "s:lg", opt, &idx)) != -1) {
		switch(ret) {
		case 's':
			aopt->act = AA_SET;
			aopt->sopt.pcm = optarg;
			break;
		case 'l':
			aopt->act = AA_LIST;
			break;
		case 'g':
			aopt->act = AA_GET;
			break;
		case '?':
			goto out;
		}
	}

out:
	if(!AM_OPT_VALID(aopt)) {
		USAGE(argc, argv);
		return -1;
	}

	return 0;
}
