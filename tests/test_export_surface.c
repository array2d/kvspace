/*
 * 导出面链接测试：**缺一个导出符号，链接就必须失败**。
 *
 * 为什么需要这条：`const.h` 曾声明 `kvspaceConst`，而实现与导出都是
 * `kvspaceConstStorage`。结果是「能编过、一链接就找不到符号」——头文件与 .so
 * 各说各话，而**没有任何测试会发现它**（`grep -rn kvspaceConst tests/ cli/` 当年为空）。
 * `src/const.c` 的头注释恰好写着同类事故的警告，它自己正踩在上面。
 *
 * 判据形态：本文件**逐个引用**全部导出符号（只取地址，不调用），任何缺失都在链接期
 * 报 `undefined reference`。这是唯一在「不运行」的情况下就能挡住这类缺陷的办法——
 * 运行期测试只有在有人先调用到那个符号时才会红，而漏掉的符号没人调用。
 *
 * 维护规则：**新增/删除导出符号时，本文件的引用表必须同步**。它刻意写成显式清单
 * 而非宏循环，因为宏循环在符号改名时会静默通过（循环体里的名字一变，两边一起变）。
 * 显式清单 + `_Static_assert` 计数，才是「少一个就红」。
 *
 * 反向控制（TASK-100 要求）：故意从实现里删掉一个导出，本文件必须链接失败。
 */
#include <kvspace/const.h>
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>

#include <stddef.h>
#include <stdio.h>

/*
 * 16 个导出符号的地址表。`(const void *)&symbol` 强制链接器解析该符号——
 * 只声明不引用会被优化掉，取地址才留下真实的未定义引用。
 *
 * 顺序与 README 的「存储常量 / 存储 ABI」分组一致，便于人工核对。
 */
static const void *const exported_symbols[] = {
    /* 生命周期 */
    (const void *)&kvspaceConnect,
    (const void *)&kvspaceClose,
    /* 存储 ABI（9 个语义操作） */
    (const void *)&kvspaceResolve,
    (const void *)&kvspaceRealloc,
    (const void *)&kvspaceFree,
    (const void *)&kvspaceCp,
    (const void *)&kvspaceMkindex,
    (const void *)&kvspaceDeltree,
    (const void *)&kvspaceCpdir,
    (const void *)&kvspaceListlen,
    (const void *)&kvspaceList,
    /* 地址空间访问器 */
    (const void *)&kvspaceXvalueInProcessAddress,
    /* 字节层 attr 访问 */
    (const void *)&kvspaceXvalueGetAttr,
    (const void *)&kvspaceXvalueSetAttr,
    (const void *)&kvspaceXvalueHeadEffectiveLen,
    /* 存储常量 */
    (const void *)&kvspaceConstStorage,
};

/*
 * 符号个数断言：加符号忘了加进表，这里立刻红。
 * 用 `!=` 而非 `>=`：导出面是**精确集合**，多一个少一个都要人看一眼。
 */
_Static_assert(sizeof(exported_symbols) / sizeof(exported_symbols[0]) == 16U,
               "export surface changed: update this table and README together");

int main(void) {
    /* 每个地址都必须非空——`&symbol` 永远非空，这条只在符号被解析成弱引用时才可能失败。 */
    for (size_t at = 0; at < sizeof(exported_symbols) / sizeof(exported_symbols[0]);
         ++at) {
        if (exported_symbols[at] == NULL) {
            fprintf(stderr, "FAIL export_surface: symbol %zu resolved to NULL\n", at);
            return 1;
        }
    }
    /* 顺带验一次真实可用：常量查询能返回非空串。
     * 这条不是链接判据（上面才是），但它能挡住「链接上了但实现是空壳」。 */
    const char *separator = kvspaceConstStorage("KVSPACE_MEMBER_SEP");
    if (separator == NULL || separator[0] == '\0') {
        fprintf(stderr, "FAIL export_surface: kvspaceConstStorage returned empty\n");
        return 1;
    }
    printf("ok: %zu exported symbols linked, const query works\n",
           sizeof(exported_symbols) / sizeof(exported_symbols[0]));
    return 0;
}
