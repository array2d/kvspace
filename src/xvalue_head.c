#include <kvspace/kvspace_xvalue.h>
#include <stdint.h>
#include <string.h>
static int tag_is_writable(uint8_t tag) {
    return tag != KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN && tag != 0u;
}
KVSPACE_PUBLIC int kvspaceXvalueGetAttr(const uint8_t *wire, uint64_t wire_length, uint8_t tag,
                          const uint8_t **out_data, uint16_t *out_length) {
    if (out_data)
        *out_data = NULL;
    if (out_length)
        *out_length = 0;
    if (!wire || wire_length < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *header = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(header);
    if (total == 0 || total > wire_length)
        return -1;
    const uint8_t *pointer = wire + KVSPACE_XVALUE_HEAD_LEN + header->bodylen;
    const uint8_t *end = pointer + kvspaceXvalueHeadAttrsLen(header);
    const uint8_t *found = NULL;
    uint16_t found_length = 0;
    while (pointer + 3 <= end) {
        const uint8_t record_tag = pointer[0];
        const uint16_t record_length =
            (uint16_t)((uint16_t)pointer[1] | ((uint16_t)pointer[2] << 8));
        if (pointer + 3 + record_length > end)
            return -1;
        if (record_tag == KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN)
            return -1;
        if (record_tag == tag && !found) {
            found = pointer + 3;
            found_length = record_length;
        }
        pointer += 3 + record_length;
    }
    if (!found)
        return -1;
    if (out_data)
        *out_data = found;
    if (out_length)
        *out_length = found_length;
    return 0;
}
KVSPACE_PUBLIC int kvspaceXvalueSetAttr(uint8_t *wire, uint64_t wire_capacity, uint8_t tag,
                          const uint8_t *data, uint16_t length,
                          uint64_t *out_new_length) {
    if (out_new_length)
        *out_new_length = 0;
    if (!wire || !out_new_length || !tag_is_writable(tag))
        return -1;
    if (length != 0 && !data)
        return -1;
    kvspaceXvalueHead *header = (kvspaceXvalueHead *)(void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(header);
    if (total == 0 || total > wire_capacity)
        return -1;
    const uint64_t attributes_offset = KVSPACE_XVALUE_HEAD_LEN + kvspaceXvalueHeadLen(header);
    uint8_t *const begin = wire + attributes_offset;
    uint8_t *const end = begin + kvspaceXvalueHeadAttrsLen(header);
    uint8_t *found = NULL;
    uint16_t found_length = 0;
    for (uint8_t *pointer = begin; pointer + 3 <= end;) {
        const uint16_t record_length =
            (uint16_t)((uint16_t)pointer[1] | ((uint16_t)pointer[2] << 8));
        if (pointer + 3 + record_length > end ||
            pointer[0] == KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN)
            return -1;
        if (pointer[0] == tag) {
            found = pointer;
            found_length = record_length;
            break;
        }
        pointer += 3 + record_length;
    }
    const int removing = (length == 0 && data == NULL);
    if (removing && !found) {
        *out_new_length = total;
        return 0;
    }
    const uint64_t old_record = found ? 3u + (uint64_t)found_length : 0u;
    const uint64_t new_record = removing ? 0u : 3u + (uint64_t)length;
    const uint64_t old_attributes = kvspaceXvalueHeadAttrsLen(header);
    const int64_t delta = (int64_t)new_record - (int64_t)old_record;
    const int64_t new_attributes = (int64_t)old_attributes + delta;
    if (new_attributes < 0 || new_attributes > UINT16_MAX)
        return -1;
    if (delta > 0 && total + (uint64_t)delta > wire_capacity)
        return -1;
    uint8_t *const from = found ? found + 3 + found_length : end;
    uint8_t *const to = from + delta;
    const uint64_t tail = (uint64_t)(end - from);
    if (tail && delta != 0)
        memmove(to, from, (size_t)tail);
    if (!removing) {
        uint8_t *write_at = found ? found : end;
        write_at[0] = tag;
        write_at[1] = (uint8_t)length;
        write_at[2] = (uint8_t)(length >> 8);
        if (length)
            memcpy(write_at + 3, data, length);
    }
    kvspaceXvalueHeadSetAttrsLen(header, (uint16_t)new_attributes);
    *out_new_length = total + (uint64_t)delta;
    return 0;
}
KVSPACE_PUBLIC int kvspaceXvalueHeadEffectiveLen(const uint8_t *wire, uint64_t wire_length,
                                  uint64_t *out) {
    if (out)
        *out = 0;
    if (!out || !wire || wire_length < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *header = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(header);
    if (total == 0 || total > wire_length)
        return -1;
    const uint8_t *record = wire + KVSPACE_XVALUE_HEAD_LEN + header->bodylen;
    const uint8_t *const end = record + kvspaceXvalueHeadAttrsLen(header);
    while (record + 3 <= end) {
        const uint16_t record_length =
            (uint16_t)((uint16_t)record[1] | ((uint16_t)record[2] << 8));
        if (record + 3 + record_length > end ||
            record[0] == KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN)
            return -1;
        record += 3 + record_length;
    }
    *out = header->bodylen;
    return 0;
}
