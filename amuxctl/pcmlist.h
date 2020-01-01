#ifndef _PCMLIST_H_
#define _PCMLIST_H_

#include <sys/queue.h>

struct pcm;
LIST_HEAD(pcmlst, pcm);

void pcmlst_dump(struct pcmlst const *plst);
int pcmlst_add(struct pcmlst *plst, void const *hint);
int pcmlst_init(struct pcmlst *plst);
void pcmlst_cleanup(struct pcmlst *plst);

#endif
