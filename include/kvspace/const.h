#ifndef KVSPACE_CONST_H
#define KVSPACE_CONST_H

/* KVSpace 常量唯一定义处：分隔符 + kind 串。
 *
 * - `#define KVSPACE_* "…"` 是值唯一来源，C/C++ 编译期引用（支持字面量拼接）。
 * - KVSPACE_KV(X) 列出全部名字，供 kvspaceConstStorage() 运行期查询生成查找表；
 *   Rust 侧用 build.rs 解析 `#define KVSPACE_* "…"` 行生成同名常量，不引入 bindgen。
 */

#define KVSPACE_PATH_SEP           "/"
#define KVSPACE_DIR_INDEX_SUF       "/"
#define KVSPACE_MEMBER_SEP          "·"
#define KVSPACE_RUNTIME_MEMBER_SEP  "\xE2\x80\xA5"


#define KVSPACE_ERR_DIR_MUST_END_WITH_SLASH "kvspace: index must end with /"
#define KVSPACE_ERR_INVALID_PATH            "kvspace: path must be absolute and canonical"

#define KVSPACE_KV(X) \
    X(KVSPACE_PATH_SEP) \
    X(KVSPACE_DIR_INDEX_SUF) \
    X(KVSPACE_MEMBER_SEP) \
    X(KVSPACE_RUNTIME_MEMBER_SEP) \
    X(KVSPACE_ERR_DIR_MUST_END_WITH_SLASH) \
    X(KVSPACE_ERR_INVALID_PATH)

/* 运行期查询：kvspaceConstStorage("KVSPACE_MEMBER_SEP") -> "·"。
 * 供 Go/Python/Rust 扩展经 C ABI 取常量，避免硬编码。
 *
 * **只认存储常量**：语言层 kind 串（int64/stringkeymap/…）归 kvlang 的
 * `kvlangConstLanguage`，不在这里——存储层不认识 langtype 这个词
 * （见 README「语言层不在这里」）。 */
const char *kvspaceConstStorage(const char *name);

#endif
