#ifndef KVSPACE_SHM_H
#define KVSPACE_SHM_H
#include <kvspace/kvspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct kvspace kvspace_t;
int kvspaceTestListLen(kvspace_t *kv, const char *prefix, bool expand_ext,
                       int resolve, int32_t *out_count);
#ifdef P2_TEST_HOOKS
void kvspaceTestSetDataSize(size_t data_size);
int64_t kvspaceTestArtAlignCheck(kvspace_t *kv);
#endif
#endif
