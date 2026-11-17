#ifndef KVSPACE_SHM_H
#define KVSPACE_SHM_H
#include <kvspace/kvspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct kvspace kvspace_t;
int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool expand_ext,
                      int resolve, int32_t *out_count);
int kvspaceShmCplist(kvspace_t *kv, const char *src, const char *dst);
#ifdef P2_TEST_HOOKS
void kvspaceTestSetDataSize(size_t data_size);
void kvspaceShmTestSetGarbageThresholdMegabytes(uint32_t megabytes);
void kvspaceShmTestSetBarrierMilliseconds(int milliseconds);
void kvspaceShmTestSetHoldDrainMilliseconds(uint32_t milliseconds);
int kvspaceShmTestAnchorOf(kvspace_t *kv, const char *parent_key,
                           int64_t *out_anchor);
#endif
#endif
