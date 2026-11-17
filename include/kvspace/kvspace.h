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
    KVSPACE_CAPACITY = -ENOSPC,
    KVSPACE_NOT_INDEX = -ENOTDIR,
    KVSPACE_HAS_CHILDREN = -ENOTEMPTY,
    KVSPACE_BACKEND = -EIO
};
void *kvspaceConnect(const char *dsn);
void kvspaceClose(void *h);
uint8_t *kvspaceXvalueInProcessAddress(void *handle);
int kvspaceResolveOffset(void *handle, const char *key,
                         const kvspace_anchor_t *anchor,
                         uint64_t *out_value_offset, char *err, uint32_t err_cap);
int kvspaceAnchorOf(void *handle, const char *directory_key,
                    kvspace_anchor_t *out_anchor, char *err, uint32_t err_cap);
int kvspaceRealloc(void *handle, const char *key, uint64_t body_length,
                   uint64_t attr_length, uint64_t *out_value_offset,
                   char *err, uint32_t err_cap);
int kvspaceFree(void *handle, const char *key, char *err, uint32_t err_cap);
int kvspaceCp(void *handle, const char *old_key, const char *new_key, char *err,
              uint32_t err_cap);
int kvspaceMkindex(void *handle, const char *key, char *err, uint32_t err_cap);
int kvspaceDeltree(void *handle, const char *key, char *err, uint32_t err_cap);
int kvspaceCpdir(void *handle, const char *source_key,
                 const char *destination_key, char *err, uint32_t err_cap);
int kvspaceListlen(void *handle, const char *key, int64_t *out_count,
                   uint32_t *out_max_name_length, char *err, uint32_t err_cap);
int kvspaceList(void *handle, const char *key, char *out_names,
                uint32_t out_capacity, uint32_t *out_length,
                uint32_t *out_count, uint64_t *out_generation, char *err,
                uint32_t err_cap);
#ifdef __cplusplus
}
#endif
#endif
