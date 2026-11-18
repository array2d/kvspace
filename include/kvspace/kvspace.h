#ifndef KVSPACE_KVSPACE_H
#define KVSPACE_KVSPACE_H 
#include <errno.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct kvspace_anchor {
    uint64_t node_offset;
    uint64_t epoch;
    uint32_t depth;
    uint32_t flags;
} kvspace_anchor_t;
#define KVSPACE_ANCHOR_FLAG_INDEX UINT32_C(1)
enum {
    KVSPACE_OK = 0,
    KVSPACE_INVALID = -EINVAL,
    KVSPACE_MISSING = -ENOENT,
    KVSPACE_ANCHOR_INVALID = -ESTALE,
    KVSPACE_CAPACITY = -ENOBUFS,
    KVSPACE_NOT_INDEX = -ENOTDIR,
    KVSPACE_HAS_CHILDREN = -ENOTEMPTY,
    KVSPACE_BACKEND = -EIO
};
void *kvspaceConnect(const char *dsn);
void kvspaceClose(void *h);
uint8_t *kvspaceXvalueInProcessAddress(void *handle);
int kvspaceResolveOffset(void *handle, const char *key,
                         const kvspace_anchor_t *anchor,
                         uint64_t *out_value_offset);
int kvspaceAnchorOf(void *handle, const char *directory_key,
                    kvspace_anchor_t *out_anchor);
int kvspaceRealloc(void *handle, const char *key, uint64_t body_length,
                   uint64_t attr_length, uint64_t *out_value_offset);
int kvspaceFree(void *handle, const char *key);
int kvspaceCp(void *handle, const char *old_key, const char *new_key);
int kvspaceMkindex(void *handle, const char *key);
int kvspaceDeltree(void *handle, const char *key);
int kvspaceCpdir(void *handle, const char *source_key,
                 const char *destination_key);
int kvspaceListlen(void *handle, const char *key, int64_t *out_count,
                   uint32_t *out_max_name_length);
int kvspaceList(void *handle, const char *key, char *out_names,
                uint32_t out_capacity, uint32_t *out_length,
                uint32_t *out_count, uint64_t *out_generation);
#ifdef __cplusplus
}
#endif
#endif
