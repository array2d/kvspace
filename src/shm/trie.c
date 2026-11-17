#include "trie.h"
#include <sched.h>
#include <string.h>

#define TRIE_INITIAL_CAPACITY UINT32_C(4)
#define TRIE_SEPARATOR_SLASH ((uint8_t)0x2F)
#define TRIE_SEPARATOR_DOT_LEAD ((uint8_t)0xC2)
#define TRIE_SEPARATOR_DOT_TRAIL ((uint8_t)0xB7)

static void trie_processor_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    sched_yield();
#endif
}

static int trie_environment_ready(const trie_env_t *env) {
    return env != NULL && env->alloc_bytes != NULL && env->base != NULL &&
           env->slab_size >= sizeof(trie_hdr_t);
}

static int trie_span_inside(const trie_env_t *env, uint64_t offset,
                            uint64_t length) {
    if (offset > env->slab_size) {
        return 0;
    }
    return env->slab_size - offset >= length;
}

static int trie_header_inside(const trie_env_t *env, uint64_t offset) {
    if (offset == 0 || (offset & UINT64_C(7)) != 0) {
        return 0;
    }
    return trie_span_inside(env, offset, sizeof(trie_hdr_t));
}

static trie_hdr_t *trie_header_pointer(const trie_env_t *env, uint64_t offset) {
    return (trie_hdr_t *)(void *)(env->base + offset);
}

static trie_ent_t *trie_table_pointer(const trie_env_t *env,
                                      const trie_hdr_t *header) {
    return (trie_ent_t *)(void *)(env->base + header->table_offset);
}

static int trie_node_check(const trie_env_t *env, uint64_t offset,
                           trie_hdr_t **out_header) {
    if (!trie_header_inside(env, offset)) {
        return KVSPACE_ANCHOR_INVALID;
    }
    trie_hdr_t *header = trie_header_pointer(env, offset);
    if (header->count > header->capacity) {
        return KVSPACE_ANCHOR_INVALID;
    }
    if (header->capacity > 0) {
        uint64_t table_bytes =
            (uint64_t)header->capacity * sizeof(trie_ent_t);
        if (!trie_span_inside(env, header->table_offset, table_bytes)) {
            return KVSPACE_ANCHOR_INVALID;
        }
    } else if (header->count != 0) {
        return KVSPACE_ANCHOR_INVALID;
    }
    if (header->name_length > 0 &&
        !trie_span_inside(env, header->name_offset, header->name_length)) {
        return KVSPACE_ANCHOR_INVALID;
    }
    if (out_header != NULL) {
        *out_header = header;
    }
    return KVSPACE_OK;
}

static int trie_node_live(const trie_env_t *env, uint64_t offset,
                          trie_hdr_t **out_header) {
    trie_hdr_t *header = NULL;
    int status = trie_node_check(env, offset, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    if ((header->flags & TRIE_FLAG_DEAD) != 0) {
        return KVSPACE_ANCHOR_INVALID;
    }
    if (out_header != NULL) {
        *out_header = header;
    }
    return KVSPACE_OK;
}

static int trie_allocate(trie_env_t *env, uint64_t bytes,
                         uint64_t *out_offset) {
    uint64_t actual = bytes == 0 ? UINT64_C(1) : bytes;
    uint64_t offset = 0;
    if (env->alloc_bytes(env->context, actual, &offset) != 0) {
        return KVSPACE_CAPACITY;
    }
    if ((offset & UINT64_C(7)) != 0) {
        return KVSPACE_BACKEND;
    }
    if (!trie_span_inside(env, offset, actual)) {
        return KVSPACE_CAPACITY;
    }
    *out_offset = offset;
    return KVSPACE_OK;
}

static int trie_name_compare(const uint8_t *left, uint16_t left_length,
                             const uint8_t *right, uint16_t right_length) {
    uint16_t common =
        left_length < right_length ? left_length : right_length;
    int order = memcmp(left, right, common);
    if (order != 0) {
        return order;
    }
    if (left_length < right_length) {
        return -1;
    }
    if (left_length > right_length) {
        return 1;
    }
    return 0;
}

static int trie_name_is_index(const uint8_t *name, uint16_t length) {
    if (length == 0) {
        return 1;
    }
    if (name[length - 1] == TRIE_SEPARATOR_SLASH) {
        return 1;
    }
    if (length >= 2 && name[length - 2] == TRIE_SEPARATOR_DOT_LEAD &&
        name[length - 1] == TRIE_SEPARATOR_DOT_TRAIL) {
        return 1;
    }
    return 0;
}

static int trie_node_is_index(const trie_env_t *env,
                              const trie_hdr_t *header) {
    if ((header->flags & TRIE_FLAG_ROOT) != 0) {
        return 1;
    }
    return trie_name_is_index(env->base + header->name_offset,
                              header->name_length);
}

static int trie_segments_parse(const char *key, int absolute,
                               trie_segment_t *out_segments,
                               int max_segments, int *out_count) {
    if (key == NULL || out_segments == NULL || out_count == NULL ||
        max_segments <= 0) {
        return KVSPACE_INVALID;
    }
    size_t length = strlen(key);
    if (length == 0 || length >= TRIE_KEY_LIMIT) {
        return KVSPACE_INVALID;
    }
    const uint8_t *bytes = (const uint8_t *)key;
    int count = 0;
    size_t position = 0;
    if (absolute) {
        if (bytes[0] != TRIE_SEPARATOR_SLASH) {
            return KVSPACE_INVALID;
        }
        out_segments[count].pointer = bytes;
        out_segments[count].length = 1;
        count++;
        position = 1;
    } else if (bytes[0] == TRIE_SEPARATOR_SLASH) {
        return KVSPACE_INVALID;
    }
    size_t segment_start = position;
    while (position < length) {
        size_t separator_length = 0;
        if (bytes[position] == TRIE_SEPARATOR_SLASH) {
            separator_length = 1;
        } else if (bytes[position] == TRIE_SEPARATOR_DOT_LEAD &&
                   position + 1 < length &&
                   bytes[position + 1] == TRIE_SEPARATOR_DOT_TRAIL) {
            separator_length = 2;
        }
        if (separator_length == 0) {
            position++;
            continue;
        }
        if (position == segment_start) {
            return KVSPACE_INVALID;
        }
        if (count >= max_segments) {
            return KVSPACE_INVALID;
        }
        out_segments[count].pointer = bytes + segment_start;
        out_segments[count].length =
            (uint16_t)(position + separator_length - segment_start);
        count++;
        position += separator_length;
        segment_start = position;
    }
    if (segment_start < length) {
        if (count >= max_segments) {
            return KVSPACE_INVALID;
        }
        out_segments[count].pointer = bytes + segment_start;
        out_segments[count].length = (uint16_t)(length - segment_start);
        count++;
    }
    if (count == 0) {
        return KVSPACE_INVALID;
    }
    *out_count = count;
    return KVSPACE_OK;
}

static int trie_entry_position(const trie_env_t *env, trie_hdr_t *header,
                               const uint8_t *name, uint16_t length,
                               uint32_t *out_position, int *out_found) {
    trie_ent_t *table = trie_table_pointer(env, header);
    uint32_t low = 0;
    uint32_t high = header->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        trie_ent_t *entry = &table[middle];
        if (!trie_span_inside(env, entry->name_offset, entry->name_length)) {
            return KVSPACE_ANCHOR_INVALID;
        }
        const uint8_t *entry_name = env->base + entry->name_offset;
        int order = trie_name_compare(entry_name, entry->name_length, name,
                                      length);
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    int found = 0;
    if (low < header->count) {
        trie_ent_t *entry = &table[low];
        if (!trie_span_inside(env, entry->name_offset, entry->name_length)) {
            return KVSPACE_ANCHOR_INVALID;
        }
        const uint8_t *entry_name = env->base + entry->name_offset;
        found = trie_name_compare(entry_name, entry->name_length, name,
                                  length) == 0;
    }
    *out_position = low;
    *out_found = found;
    return KVSPACE_OK;
}

static int trie_table_reserve(trie_env_t *env, trie_hdr_t *header,
                              uint32_t needed_capacity) {
    if (needed_capacity <= header->capacity) {
        return KVSPACE_OK;
    }
    uint32_t capacity = header->capacity == 0 ? TRIE_INITIAL_CAPACITY
                                              : header->capacity;
    while (capacity < needed_capacity) {
        if (capacity > UINT32_MAX / 2) {
            return KVSPACE_CAPACITY;
        }
        capacity *= 2;
    }
    uint64_t table_bytes = (uint64_t)capacity * sizeof(trie_ent_t);
    uint64_t table_offset = 0;
    int status = trie_allocate(env, table_bytes, &table_offset);
    if (status != KVSPACE_OK) {
        return status;
    }
    if (header->count > 0) {
        uint64_t used_bytes = (uint64_t)header->count * sizeof(trie_ent_t);
        if (!trie_span_inside(env, header->table_offset, used_bytes)) {
            return KVSPACE_ANCHOR_INVALID;
        }
        memcpy(env->base + table_offset, env->base + header->table_offset,
               (size_t)used_bytes);
    }
    header->table_offset = table_offset;
    header->capacity = capacity;
    header->struct_gen++;
    return KVSPACE_OK;
}

static int trie_node_create(trie_env_t *env, const uint8_t *name,
                            uint16_t length, uint32_t flags,
                            uint64_t *out_offset) {
    uint64_t node_offset = 0;
    int status = trie_allocate(env, sizeof(trie_hdr_t), &node_offset);
    if (status != KVSPACE_OK) {
        return status;
    }
    uint64_t name_offset = 0;
    if (length > 0) {
        status = trie_allocate(env, length, &name_offset);
        if (status != KVSPACE_OK) {
            return status;
        }
        memcpy(env->base + name_offset, name, length);
    }
    trie_hdr_t *header = trie_header_pointer(env, node_offset);
    header->value = TRIE_VALUE_ABSENT;
    header->table_offset = 0;
    header->count = 0;
    header->capacity = 0;
    header->lock = 0;
    header->flags = flags;
    header->struct_gen = 0;
    header->value_gen = 0;
    header->name_offset = name_offset;
    header->name_length = length;
    header->pad = 0;
    *out_offset = node_offset;
    return KVSPACE_OK;
}

static int trie_edge_insert(trie_env_t *env, trie_hdr_t *parent,
                            uint64_t child_offset) {
    trie_hdr_t *child = trie_header_pointer(env, child_offset);
    if (!trie_span_inside(env, child->name_offset, child->name_length)) {
        return KVSPACE_ANCHOR_INVALID;
    }
    const uint8_t *name = env->base + child->name_offset;
    uint16_t length = child->name_length;
    uint32_t position = 0;
    int found = 0;
    int status =
        trie_entry_position(env, parent, name, length, &position, &found);
    if (status != KVSPACE_OK) {
        return status;
    }
    if (found) {
        return KVSPACE_INVALID;
    }
    status = trie_table_reserve(env, parent, parent->count + 1);
    if (status != KVSPACE_OK) {
        return status;
    }
    trie_ent_t *table = trie_table_pointer(env, parent);
    if (position < parent->count) {
        memmove(&table[position + 1], &table[position],
                (size_t)(parent->count - position) * sizeof(trie_ent_t));
    }
    table[position].name_offset = child->name_offset;
    table[position].name_length = child->name_length;
    table[position].pad = 0;
    table[position].child_offset = child_offset;
    parent->count++;
    parent->struct_gen++;
    return KVSPACE_OK;
}

static int trie_write_from(trie_env_t *env, uint64_t start_offset,
                           const trie_segment_t *segments, int count,
                           int first_segment, uint64_t value,
                           uint64_t *out_node) {
    uint64_t current_offset = start_offset;
    trie_hdr_t *current_header = NULL;
    int status = trie_node_live(env, current_offset, &current_header);
    if (status != KVSPACE_OK) {
        return status;
    }
    for (int index = first_segment; index < count; index++) {
        if (current_header->name_length > 0 &&
            !trie_node_is_index(env, current_header)) {
            return KVSPACE_NOT_INDEX;
        }
        uint32_t position = 0;
        int found = 0;
        status = trie_entry_position(env, current_header,
                                     segments[index].pointer,
                                     segments[index].length, &position,
                                     &found);
        if (status != KVSPACE_OK) {
            return status;
        }
        uint64_t child_offset = 0;
        if (found) {
            child_offset =
                trie_table_pointer(env, current_header)[position].child_offset;
        } else {
            status = trie_node_create(env, segments[index].pointer,
                                      segments[index].length, 0,
                                      &child_offset);
            if (status != KVSPACE_OK) {
                return status;
            }
            if (index + 1 < count && env->make_index_value != NULL) {
                uint64_t initial_value = TRIE_VALUE_ABSENT;
                if (env->make_index_value(env->context, &initial_value) != 0) {
                    return KVSPACE_BACKEND;
                }
                trie_hdr_t *created = trie_header_pointer(env, child_offset);
                created->value = initial_value;
                created->value_gen++;
            }
            status = trie_edge_insert(
                env, trie_header_pointer(env, current_offset), child_offset);
            if (status != KVSPACE_OK) {
                return status;
            }
        }
        current_offset = child_offset;
        status = trie_node_live(env, current_offset, &current_header);
        if (status != KVSPACE_OK) {
            return status;
        }
    }
    current_header->value = value;
    current_header->value_gen++;
    if (out_node != NULL) {
        *out_node = current_offset;
    }
    return KVSPACE_OK;
}

static int trie_find_from(trie_env_t *env, uint64_t start_offset,
                          const trie_segment_t *segments, int count,
                          int first_segment, uint64_t *out_node) {
    uint64_t current_offset = start_offset;
    trie_hdr_t *current_header = NULL;
    int status = trie_node_live(env, current_offset, &current_header);
    if (status != KVSPACE_OK) {
        return status;
    }
    for (int index = first_segment; index < count; index++) {
        uint32_t position = 0;
        int found = 0;
        status = trie_entry_position(env, current_header,
                                     segments[index].pointer,
                                     segments[index].length, &position,
                                     &found);
        if (status != KVSPACE_OK) {
            return status;
        }
        if (!found) {
            return KVSPACE_MISSING;
        }
        current_offset =
            trie_table_pointer(env, current_header)[position].child_offset;
        status = trie_node_live(env, current_offset, &current_header);
        if (status != KVSPACE_OK) {
            return status;
        }
    }
    *out_node = current_offset;
    return KVSPACE_OK;
}

int trie_root_create(trie_env_t *env, uint64_t *out_root) {
    if (!trie_environment_ready(env) || out_root == NULL) {
        return KVSPACE_INVALID;
    }
    uint64_t offset = 0;
    int status = trie_allocate(env, sizeof(trie_hdr_t), &offset);
    if (status != KVSPACE_OK) {
        return status;
    }
    if (offset == 0) {
        status = trie_allocate(env, sizeof(trie_hdr_t), &offset);
        if (status != KVSPACE_OK) {
            return status;
        }
    }
    trie_hdr_t *header = trie_header_pointer(env, offset);
    header->value = TRIE_VALUE_ABSENT;
    header->table_offset = 0;
    header->count = 0;
    header->capacity = 0;
    header->lock = 0;
    header->flags = TRIE_FLAG_ROOT;
    header->struct_gen = 0;
    header->value_gen = 0;
    header->name_offset = 0;
    header->name_length = 0;
    header->pad = 0;
    *out_root = offset;
    return KVSPACE_OK;
}

int trie_key_segments(const char *key, trie_segment_t *out_segments,
                      int max_segments, int *out_count) {
    if (out_count == NULL) {
        return KVSPACE_INVALID;
    }
    *out_count = 0;
    return trie_segments_parse(key, 1, out_segments, max_segments, out_count);
}

int trie_find_absolute(trie_env_t *env, uint64_t root, const char *key,
                       uint64_t *out_node) {
    if (!trie_environment_ready(env) || out_node == NULL) {
        return KVSPACE_INVALID;
    }
    trie_segment_t segments[TRIE_MAX_SEGMENTS];
    int count = 0;
    int status =
        trie_segments_parse(key, 1, segments, TRIE_MAX_SEGMENTS, &count);
    if (status != KVSPACE_OK) {
        return status;
    }
    return trie_find_from(env, root, segments, count, 1, out_node);
}

int trie_find_relative(trie_env_t *env, uint64_t anchor, const char *suffix,
                       uint64_t *out_node) {
    if (!trie_environment_ready(env) || out_node == NULL) {
        return KVSPACE_INVALID;
    }
    trie_segment_t segments[TRIE_MAX_SEGMENTS];
    int count = 0;
    int status =
        trie_segments_parse(suffix, 0, segments, TRIE_MAX_SEGMENTS, &count);
    if (status != KVSPACE_OK) {
        return status;
    }
    return trie_find_from(env, anchor, segments, count, 0, out_node);
}

int trie_write(trie_env_t *env, uint64_t root, const char *key, uint64_t value,
               uint64_t *out_node) {
    if (!trie_environment_ready(env)) {
        return KVSPACE_INVALID;
    }
    trie_segment_t segments[TRIE_MAX_SEGMENTS];
    int count = 0;
    int status =
        trie_segments_parse(key, 1, segments, TRIE_MAX_SEGMENTS, &count);
    if (status != KVSPACE_OK) {
        return status;
    }
    return trie_write_from(env, root, segments, count, 1, value, out_node);
}

int trie_write_relative(trie_env_t *env, uint64_t anchor, const char *suffix,
                        uint64_t value, uint64_t *out_node) {
    if (!trie_environment_ready(env)) {
        return KVSPACE_INVALID;
    }
    trie_segment_t segments[TRIE_MAX_SEGMENTS];
    int count = 0;
    int status =
        trie_segments_parse(suffix, 0, segments, TRIE_MAX_SEGMENTS, &count);
    if (status != KVSPACE_OK) {
        return status;
    }
    return trie_write_from(env, anchor, segments, count, 0, value, out_node);
}

int trie_remove(trie_env_t *env, uint64_t root, const char *key) {
    if (!trie_environment_ready(env)) {
        return KVSPACE_INVALID;
    }
    trie_segment_t segments[TRIE_MAX_SEGMENTS];
    int count = 0;
    int status =
        trie_segments_parse(key, 1, segments, TRIE_MAX_SEGMENTS, &count);
    if (status != KVSPACE_OK) {
        return status;
    }
    trie_hdr_t *root_header = NULL;
    status = trie_node_live(env, root, &root_header);
    if (status != KVSPACE_OK) {
        return status;
    }
    if (count == 1) {
        if (root_header->count > 0) {
            return TRIE_HAS_CHILDREN;
        }
        root_header->value = TRIE_VALUE_ABSENT;
        root_header->value_gen++;
        return KVSPACE_OK;
    }
    uint64_t parent_offset = root;
    trie_hdr_t *parent_header = root_header;
    for (int index = 1; index < count - 1; index++) {
        uint32_t position = 0;
        int found = 0;
        status = trie_entry_position(env, parent_header,
                                     segments[index].pointer,
                                     segments[index].length, &position,
                                     &found);
        if (status != KVSPACE_OK) {
            return status;
        }
        if (!found) {
            return KVSPACE_MISSING;
        }
        parent_offset =
            trie_table_pointer(env, parent_header)[position].child_offset;
        status = trie_node_live(env, parent_offset, &parent_header);
        if (status != KVSPACE_OK) {
            return status;
        }
    }
    uint32_t position = 0;
    int found = 0;
    status = trie_entry_position(env, parent_header,
                                 segments[count - 1].pointer,
                                 segments[count - 1].length, &position,
                                 &found);
    if (status != KVSPACE_OK) {
        return status;
    }
    if (!found) {
        return KVSPACE_MISSING;
    }
    trie_ent_t *parent_table = trie_table_pointer(env, parent_header);
    uint64_t node_offset = parent_table[position].child_offset;
    trie_hdr_t *node_header = NULL;
    status = trie_node_live(env, node_offset, &node_header);
    if (status != KVSPACE_OK) {
        return status;
    }
    if (node_header->count > 0) {
        return TRIE_HAS_CHILDREN;
    }
    node_header->value = TRIE_VALUE_ABSENT;
    node_header->value_gen++;
    node_header->flags |= TRIE_FLAG_DEAD;
    if (position + 1 < parent_header->count) {
        memmove(&parent_table[position], &parent_table[position + 1],
                (size_t)(parent_header->count - position - 1) *
                    sizeof(trie_ent_t));
    }
    parent_header->count--;
    parent_header->struct_gen++;
    return KVSPACE_OK;
}

int trie_node_header(trie_env_t *env, uint64_t offset,
                     trie_hdr_t **out_header) {
    if (!trie_environment_ready(env) || out_header == NULL) {
        return KVSPACE_INVALID;
    }
    return trie_node_check(env, offset, out_header);
}

int trie_child_count(trie_env_t *env, uint64_t node, uint32_t *out_count) {
    if (!trie_environment_ready(env) || out_count == NULL) {
        return KVSPACE_INVALID;
    }
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    *out_count = header->count;
    return KVSPACE_OK;
}

int trie_longest_child_name(trie_env_t *env, uint64_t node,
                            uint16_t *out_length) {
    if (!trie_environment_ready(env) || out_length == NULL) {
        return KVSPACE_INVALID;
    }
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    trie_ent_t *table = trie_table_pointer(env, header);
    uint16_t longest = 0;
    for (uint32_t index = 0; index < header->count; index++) {
        trie_ent_t *entry = &table[index];
        if (!trie_span_inside(env, entry->name_offset, entry->name_length)) {
            return KVSPACE_ANCHOR_INVALID;
        }
        if (entry->name_length > longest) {
            longest = entry->name_length;
        }
    }
    *out_length = longest;
    return KVSPACE_OK;
}

int trie_list_children(trie_env_t *env, uint64_t node, trie_visit_t visit,
                       void *context, uint32_t *out_count) {
    if (!trie_environment_ready(env) || visit == NULL) {
        return KVSPACE_INVALID;
    }
    if (out_count != NULL) {
        *out_count = 0;
    }
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    trie_ent_t *table = trie_table_pointer(env, header);
    uint32_t index = 0;
    for (; index < header->count; index++) {
        trie_ent_t *entry = &table[index];
        if (!trie_span_inside(env, entry->name_offset, entry->name_length)) {
            return KVSPACE_ANCHOR_INVALID;
        }
        int visit_status =
            visit(context, env->base + entry->name_offset,
                  entry->name_length, entry->child_offset);
        if (visit_status != 0) {
            if (out_count != NULL) {
                *out_count = index;
            }
            return visit_status;
        }
    }
    if (out_count != NULL) {
        *out_count = index;
    }
    return KVSPACE_OK;
}

static int trie_subtree_walk(trie_env_t *env, uint64_t node, char *suffix,
                             size_t suffix_length,
                             trie_subtree_visit_t visit, void *context,
                             uint32_t *out_count) {
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    int visit_status = visit(context, suffix, node, header->value);
    if (visit_status != 0) {
        return visit_status;
    }
    (*out_count)++;
    trie_ent_t *table = trie_table_pointer(env, header);
    for (uint32_t index = 0; index < header->count; index++) {
        trie_ent_t *entry = &table[index];
        if (!trie_span_inside(env, entry->name_offset, entry->name_length)) {
            return KVSPACE_ANCHOR_INVALID;
        }
        if (suffix_length + entry->name_length >= TRIE_KEY_LIMIT) {
            return KVSPACE_INVALID;
        }
        memcpy(suffix + suffix_length, env->base + entry->name_offset,
               entry->name_length);
        suffix[suffix_length + entry->name_length] = '\0';
        status = trie_subtree_walk(env, entry->child_offset, suffix,
                                   suffix_length + entry->name_length, visit,
                                   context, out_count);
        if (status != 0) {
            return status;
        }
    }
    return KVSPACE_OK;
}

int trie_subtree_visit(trie_env_t *env, uint64_t start,
                       trie_subtree_visit_t visit, void *context,
                       uint32_t *out_count) {
    if (!trie_environment_ready(env) || visit == NULL) {
        return KVSPACE_INVALID;
    }
    if (out_count != NULL) {
        *out_count = 0;
    }
    char suffix[TRIE_KEY_LIMIT];
    suffix[0] = '\0';
    uint32_t count = 0;
    int status = trie_subtree_walk(env, start, suffix, 0, visit, context,
                                   &count);
    if (out_count != NULL) {
        *out_count = count;
    }
    return status;
}

int trie_child_at(trie_env_t *env, uint64_t node, uint32_t index,
                  const uint8_t **out_name, uint16_t *out_name_length,
                  uint64_t *out_child) {
    if (!trie_environment_ready(env) || out_name == NULL ||
        out_name_length == NULL || out_child == NULL) {
        return KVSPACE_INVALID;
    }
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    if (index >= header->count) {
        return KVSPACE_INVALID;
    }
    trie_ent_t *entry = &trie_table_pointer(env, header)[index];
    if (!trie_span_inside(env, entry->name_offset, entry->name_length)) {
        return KVSPACE_ANCHOR_INVALID;
    }
    *out_name = env->base + entry->name_offset;
    *out_name_length = entry->name_length;
    *out_child = entry->child_offset;
    return KVSPACE_OK;
}

int trie_value_get(trie_env_t *env, uint64_t node, uint64_t *out_value) {
    if (!trie_environment_ready(env) || out_value == NULL) {
        return KVSPACE_INVALID;
    }
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    *out_value = header->value;
    return KVSPACE_OK;
}

int trie_value_set(trie_env_t *env, uint64_t node, uint64_t value) {
    if (!trie_environment_ready(env)) {
        return KVSPACE_INVALID;
    }
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    header->value = value;
    header->value_gen++;
    return KVSPACE_OK;
}

int trie_generations(trie_env_t *env, uint64_t node, uint32_t *out_struct_gen,
                     uint32_t *out_value_gen) {
    if (!trie_environment_ready(env) || out_struct_gen == NULL ||
        out_value_gen == NULL) {
        return KVSPACE_INVALID;
    }
    trie_hdr_t *header = NULL;
    int status = trie_node_live(env, node, &header);
    if (status != KVSPACE_OK) {
        return status;
    }
    *out_struct_gen = header->struct_gen;
    *out_value_gen = header->value_gen;
    return KVSPACE_OK;
}

void trie_node_lock(trie_env_t *env, uint64_t node) {
    trie_hdr_t *header = trie_header_pointer(env, node);
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&header->lock, &expected, UINT32_C(1),
                                        0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
        while (__atomic_load_n(&header->lock, __ATOMIC_RELAXED) != 0) {
            trie_processor_relax();
        }
    }
}

void trie_node_unlock(trie_env_t *env, uint64_t node) {
    trie_hdr_t *header = trie_header_pointer(env, node);
    __atomic_store_n(&header->lock, UINT32_C(0), __ATOMIC_RELEASE);
}
