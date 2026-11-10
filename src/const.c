/* 存储常量运行期查询：供 Go/Python/Rust 扩展经 C ABI 取常量，避免各语言硬编码
 * 一份名字。语言常量（kind 串）不在这里——它们在 kvlang 的 `kvlangConstLanguage`。
 *
 * 历史上 `src/codec_abi.c` 曾把这份实现与 `kvspaceNew*` 放在一起，而该文件在
 * 删 adapter 层时丢过一次：声明还在 `const.h` 里，于是变成「能编过、一链接就找
 * 不到符号」的陷阱。它对外的导出标注是 `KVSPACE_PUBLIC`——缺了它，生产构建的
 * `-fvisibility=hidden` 会把符号藏掉，`nm -D` 里看不到、但头里仍宣称有。 */
#include <kvspace/const.h>
#include <kvspace/kvspace_xvalue.h> /* KVSPACE_PUBLIC */

#include <stddef.h>
#include <string.h>

KVSPACE_PUBLIC const char *kvspaceConstStorage(const char *name) {
    static const struct { const char *key; const char *val; } table[] = {
#define KVSPACE_CONST_ENTRY(name) { #name, name },
        KVSPACE_KV(KVSPACE_CONST_ENTRY)
#undef KVSPACE_CONST_ENTRY
    };
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(table[i].key, name) == 0)
            return table[i].val;
    return NULL;
}
