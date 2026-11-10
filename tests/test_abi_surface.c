/* test_abi_surface.c — ABI 表面回归：只允许 kvspace.h 的十二个符号存在，
 * 旧符号族必须彻底消失。
 *
 * 为什么需要这条测试：这一轮把旧 ABI 一层层拆掉时，旧符号是**逐个**消失的，
 * 每拆一个都可能有人（或某次 rebase）把声明与实现顺手塞回来。而「塞回来」在
 * 动态链接下极难发现——多导出一个符号不会有任何编译错误，只要没人 dlsym 它，
 * 它就静静躺着；等到有人误用时才以参数错位的形式炸。所以把「导出了哪些名字」
 * 钉成一条测试，比任何注释都可靠。
 *
 * 测试只做 dlsym：不加生产符号、不改前端 vtable、不执行任何 kvspace 调用。
 *
 * 用法：
 *   test_abi_surface <frontend.so> [backend.so]
 * 后端路径也可从环境变量 KVSPACE_BACKEND 取。缺后端时打印 SKIP 并以 0 退出，
 * 但 SKIP 不是 PASS——输出里明确写出来，避免被当成「验过了」。
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

/* kvspace.h 对外承诺的十二个符号：六个存储操作 + 生命周期 + 目录 + 地址访问器。
 * 这份清单必须与 include/kvspace/kvspace.h 的函数声明逐条一致——多一个少一个
 * 都是 ABI 变更，不是笔误。 */
static const char *const REQUIRED[] = {
    "kvspaceConnect",
    "kvspaceClose",
    "kvspaceXvalueInProcessAddress",
    "kvspaceResolve",
    "kvspaceRealloc",
    "kvspaceFree",
    "kvspaceCp",
    "kvspaceMkindex",
    "kvspaceDeltree",
    "kvspaceCpdir",
    "kvspaceListlen",
    "kvspaceList",
};
#define REQUIRED_COUNT (sizeof REQUIRED / sizeof REQUIRED[0])

/* 已删除的旧符号族。它们必须**一个都 dlsym 不到**。
 *
 * 注意 `kvspaceMkindex` 不在这一列：它在新旧两代同名但签名不同（旧五参带
 * capacity、新四参没有），名字保留、签名换了。那种「同名换签名」的变更无法靠
 * dlsym 检出——dlsym 只看名字。它由探测闸（frontend_storage.c 的合取判据）
 * 在运行期挡：四个新独有符号没凑齐就不敢按新签名调用。
 *
 * 同样**不在**这一列：`kvspaceNew*`（NewPtr/NewChar/NewBool/NewInt64/
 * NewFloat64）与 `kvspaceXvalueHeadNew*` 构造器族。它们是**值构造器**，不是
 * 存储操作——用户裁决里明确保留。别因为名字里带 New 就把它们当旧符号删掉：
 * 删了 kvlang 就造不出值。 */
static const char *const RETIRED[] = {
    "kvspaceGet",
    "kvspaceSetValue",
    "kvspaceSetPart",
    "kvspaceWriteInPlace",
    "kvspaceWriteNewPlace",
    "kvspaceListLen",   /* 大写 L：旧版；新版是小写 l 的 kvspaceListlen */
    "kvspaceListAt",
    "kvspaceDel",       /* 旧批量删；新 ABI 用 kvspaceFree */
    "kvspaceDelTree",   /* 大写 T：旧版；新版是小写 t 的 kvspaceDeltree */
    "kvspaceCpTree",
    "kvspaceCpList",
    "kvspaceWatch",
    "kvspaceViewSweep",
    "kvspaceViewAck",
    "kvspaceViewQuiesce",
    "kvspaceViewResolvePrefix",
    "kvspaceViewResolve",
    "kvspaceViewFromRef",
    "kvspaceViewResolveFrom",
    "kvspaceViewCheck",
    "kvspaceViewWrite",
    "kvspaceViewDel",
    "kvspaceViewRefresh",
    "kvspaceMkindexExt",
    "kvspaceRmindexExt",
    "kvspaceGetHead",
    "kvspaceGetPart",
    "kvspaceSetPartByRef",
    "kvspaceGetByRef",
    "kvspaceSetValueByRef",
    "kvspaceResolveRef",
    "kvspaceXvalueBodyView",
};
#define RETIRED_COUNT (sizeof RETIRED / sizeof RETIRED[0])

/* 对一个已 dlopen 的对象做两轮断言。who 只用于报错定位。 */
static void check_surface(void *dl, const char *who) {
    for (size_t i = 0; i < REQUIRED_COUNT; i++) {
        CHECK(dlsym(dl, REQUIRED[i]) != NULL,
              "%s: 缺少必需符号 %s（kvspace.h 承诺了它）", who, REQUIRED[i]);
    }
    for (size_t i = 0; i < RETIRED_COUNT; i++) {
        void *sym = dlsym(dl, RETIRED[i]);
        CHECK(sym == NULL,
              "%s: 旧符号 %s 又出现了——它已从 ABI 删除，"
              "重新导出会让误用者以参数错位的方式静默失败",
              who, RETIRED[i]);
    }
}

static void *open_library(const char *path, const char *who) {
    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        fprintf(stderr, "%s: dlopen %s 失败: %s\n", who, path, dlerror());
        return NULL;
    }
    return dl;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <frontend.so> [backend.so]\n", argv[0]);
        return 2;
    }

    /* 前端：必须存在，缺了就真失败（不是 SKIP）。 */
    void *frontend = open_library(argv[1], "frontend");
    if (!frontend)
        return 2;
    check_surface(frontend, "frontend");
    dlclose(frontend);
    printf("前端: %zu 必需符号齐全, %zu 旧符号均为 NULL\n",
           (size_t)REQUIRED_COUNT, (size_t)RETIRED_COUNT);

    /* 后端：可选。没有就打印 SKIP 并以 **77** 退出——ctest 的 SKIP_RETURN_CODE。
     *
     * 为什么不返回 0：返回 0 时 ctest 显示 `Passed`，汇总层面就成了「全绿」，
     * 「后端根本没验」被彻底掩盖。只靠 stdout 写 SKIP 没用——没人会为了一次
     * 通过的构建去读每行输出。退出码是唯一不依赖人眼的信号。 */
    const char *backend_path = argc >= 3 ? argv[2] : getenv("KVSPACE_BACKEND");
    if (!backend_path || !backend_path[0]) {
        printf("SKIP 后端: 未提供（argv[2] 或 KVSPACE_BACKEND），"
               "只验了前端\n");
        if (failures) {
            fprintf(stderr, "%d failure(s)\n", failures);
            return 1;
        }
        return 77;
    } else {
        void *backend = open_library(backend_path, "backend");
        if (!backend)
            return 2;
        check_surface(backend, "backend");
        dlclose(backend);
        printf("后端: %s —— %zu 必需符号齐全, %zu 旧符号均为 NULL\n",
               backend_path, (size_t)REQUIRED_COUNT, (size_t)RETIRED_COUNT);
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
