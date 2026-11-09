#ifndef KVSPACE_ANCHOR_H
#define KVSPACE_ANCHOR_H

/*
 * parent_key_anchor 的线格式：
 *   anchor = (parent_id << 32) | depth
 *   parent_id：父目录的 ART 节点 id（30 位）
 *   depth    ：站在该节点时，绝对 key 已消费的字节数
 *   0 = root
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVSPACE_ANCHOR_ROOT UINT64_C(0)

static inline uint64_t kvspaceAnchorMake(uint32_t parent_id, uint32_t depth) {
    return ((uint64_t)parent_id << 32) | (uint64_t)depth;
}

static inline uint32_t kvspaceAnchorParentId(uint64_t anchor) {
    return (uint32_t)(anchor >> 32);
}

static inline uint32_t kvspaceAnchorDepth(uint64_t anchor) {
    return (uint32_t)(anchor & UINT32_MAX);
}

#ifdef __cplusplus
}
#endif

#endif
