#include <kvspace/kvspace_xvalue.h>

#include <stdint.h>
#include <string.h>

/* 存储层的字节读写：只认 head 布局与 attr 记录，不解释任何 tag 的语义。
   构造器（写 langtype）在 kvlang 仓的 `src/xvalue.c`，不在本仓。 */

#define KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN 2u

static int tag_is_writable(uint8_t tag) {
    return tag != KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN && tag != 0u;
}

KVSPACE_PUBLIC int kvspaceXvalueGetAttr(const uint8_t *wire, uint64_t wire_len, uint8_t tag,
                          const uint8_t **out_data, uint16_t *out_len) {
    if (out_data)
        *out_data = NULL;
    if (out_len)
        *out_len = 0;
    if (!wire || wire_len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0 || total > wire_len)
        return -1;
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + h->bodylen;
    const uint8_t *end = p + kvspaceXvalueHeadAttrsLen(h);
    /* 必须**扫完整个 attr 区**再决定返回值：命中目标 tag 只是「找到了」，不是
     * 「wire 合法」。早先这里命中即 return，于是「tag 1 在前、tag 2 在后」的
     * wire 会被当成合法读出来——而 tag 2 是退役编号，出现即说明写读双方理解
     * 不一致，整条 wire 都不能采信。位置无关：tag 2 在目标之前或之后都必须拒。 */
    const uint8_t *found = NULL;
    uint16_t found_len = 0;
    while (p + 3 <= end) {
        const uint8_t record_tag = p[0];
        const uint16_t record_len =
            (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        if (p + 3 + record_len > end)
            return -1;
        if (record_tag == KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN)
            return -1;
        if (record_tag == tag && !found) {
            found = p + 3;
            found_len = record_len;
        }
        p += 3 + record_len;
    }
    if (!found)
        return -1;
    if (out_data)
        *out_data = found;
    if (out_len)
        *out_len = found_len;
    return 0;
}

KVSPACE_PUBLIC int kvspaceXvalueSetAttr(uint8_t *wire, uint64_t wire_capacity, uint8_t tag,
                          const uint8_t *data, uint16_t len,
                          uint64_t *out_new_length) {
    if (out_new_length)
        *out_new_length = 0;
    if (!wire || !out_new_length || !tag_is_writable(tag))
        return -1;
    /* len == 0 且 data == NULL 是删除；给了 len 就必须给 data。 */
    if (len != 0 && !data)
        return -1;

    kvspaceXvalueHead *h = (kvspaceXvalueHead *)(void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0 || total > wire_capacity)
        return -1;
    const uint64_t attrs_at = KVSPACE_XVALUE_HEAD_LEN + kvspaceXvalueHeadLen(h);
    uint8_t *const begin = wire + attrs_at;
    uint8_t *const end = begin + kvspaceXvalueHeadAttrsLen(h);

    /* 先扫一遍：确定 tag 在不在，同时确认整段 attr 自洽、不含 tag 2。
     * 「先算清楚再动手」是这里唯一可行的写法——边扫边改会让后面的记录位置
     * 全部失效；而且容量检查必须在任何写入之前完成，失败时 wire 必须原样不动。 */
    uint8_t *found = NULL;
    uint16_t found_len = 0;
    for (uint8_t *p = begin; p + 3 <= end;) {
        const uint16_t record_len =
            (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        if (p + 3 + record_len > end ||
            p[0] == KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN)
            return -1;
        if (p[0] == tag) {
            found = p;
            found_len = record_len;
            break;
        }
        p += 3 + record_len;
    }

    const int removing = (len == 0 && data == NULL);
    if (removing && !found) {
        /* 删一个不存在的 tag：幂等成功，wire 原样。 */
        *out_new_length = total;
        return 0;
    }

    const uint64_t old_record = found ? 3u + (uint64_t)found_len : 0u;
    const uint64_t new_record = removing ? 0u : 3u + (uint64_t)len;
    const uint64_t old_attrs = kvspaceXvalueHeadAttrsLen(h);
    const int64_t delta = (int64_t)new_record - (int64_t)old_record;
    const int64_t new_attrs = (int64_t)old_attrs + delta;
    if (new_attrs < 0 || new_attrs > UINT16_MAX)
        return -1;
    if (delta > 0 && total + (uint64_t)delta > wire_capacity)
        return -1;

    /* 移动的永远是 found 之后的整段；追加时 found 为空，起点就是 end。 */
    uint8_t *const from = found ? found + 3 + found_len : end;
    uint8_t *const to = from + delta;
    const uint64_t tail = (uint64_t)(end - from);
    if (tail && delta != 0)
        memmove(to, from, (size_t)tail);
    if (!removing) {
        uint8_t *write_at = found ? found : end;
        write_at[0] = tag;
        write_at[1] = (uint8_t)len;
        write_at[2] = (uint8_t)(len >> 8);
        if (len)
            memcpy(write_at + 3, data, len);
    }
    kvspaceXvalueHeadSetAttrsLen(h, (uint16_t)new_attrs);
    *out_new_length = total + (uint64_t)delta;
    return 0;
}

/* 有效长度 = bodylen；保留入口给调用方统一走 accessor。
   读之前必须确认整条 wire 自洽（padding 为零、总长不越界、attr 区无 tag 2），
   否则「长度」这个数字是从一条坏 wire 上读出来的，不该被采信。 */
KVSPACE_PUBLIC int kvspaceXvalueHeadEffectiveLen(const uint8_t *wire, uint64_t wire_len,
                                  uint64_t *out) {
    if (out)
        *out = 0;
    if (!out || !wire || wire_len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0 || total > wire_len)
        return -1;
    const uint8_t *record = wire + KVSPACE_XVALUE_HEAD_LEN + h->bodylen;
    const uint8_t *const end = record + kvspaceXvalueHeadAttrsLen(h);
    while (record + 3 <= end) {
        const uint16_t record_len =
            (uint16_t)((uint16_t)record[1] | ((uint16_t)record[2] << 8));
        if (record + 3 + record_len > end ||
            record[0] == KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN)
            return -1;
        record += 3 + record_len;
    }
    *out = h->bodylen;
    return 0;
}
