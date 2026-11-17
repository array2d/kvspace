#ifndef KVSPACE_TEST_HOOKS_H
#define KVSPACE_TEST_HOOKS_H
#include <kvspace/kvspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct kvspace kvspace_t;
#ifdef RECOVERY_TEST_HOOKS
void kvspaceTestSetDataSize(size_t data_size);
int64_t kvspaceTestArtAlignCheck(kvspace_t *store);
#endif
#endif
