#ifndef _OPT_H_
#define _OPT_H_

enum am_act {
	AA_INVAL,
	AA_LIST,
	AA_SET,
	AA_GET,
};

struct am_sopt {
	char const *pcm;
};

struct am_opt {
	enum am_act act;
	union {
		struct am_sopt sopt;
	};
};

int parse_args(struct am_opt *aopt, int argc, char *argv[]);

#endif
