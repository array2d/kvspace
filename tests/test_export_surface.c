#include <kvspace/const.h>
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static const void *const exported_symbols[] = {
    (const void *)&kvspaceConnect,
    (const void *)&kvspaceClose,
    (const void *)&kvspaceResolveOffset,
    (const void *)&kvspaceAnchorOf,
    (const void *)&kvspaceRealloc,
    (const void *)&kvspaceFree,
    (const void *)&kvspaceCp,
    (const void *)&kvspaceMkindex,
    (const void *)&kvspaceDeltree,
    (const void *)&kvspaceCpdir,
    (const void *)&kvspaceListlen,
    (const void *)&kvspaceList,
    (const void *)&kvspaceXvalueInProcessAddress,
    (const void *)&kvspaceXvalueGetAttr,
    (const void *)&kvspaceXvalueSetAttr,
    (const void *)&kvspaceXvalueHeadEffectiveLen,
    (const void *)&kvspaceConstStorage,
};
_Static_assert(sizeof(exported_symbols) / sizeof(exported_symbols[0]) == 17U,
               "export surface changed: update this table and README together");
int main(void) {
    for (size_t at = 0; at < sizeof(exported_symbols) / sizeof(exported_symbols[0]);
         ++at) {
        if (exported_symbols[at] == NULL) {
            fprintf(stderr, "FAIL export_surface: symbol %zu resolved to NULL\n", at);
            return 1;
        }
    }
    const char *directory_suffix = kvspaceConstStorage("KVSPACE_DIR_INDEX_SUF");
    const char *separator = kvspaceConstStorage("KVSPACE_MEMBER_SEP");
    if (directory_suffix == NULL || strcmp(directory_suffix, "/") != 0 ||
        separator == NULL || strcmp(separator, "·") != 0) {
        fprintf(stderr, "FAIL export_surface: const values changed\n");
        return 1;
    }
    if (kvspaceConstStorage("KVSPACE_PATH_SEP") != NULL ||
        kvspaceConstStorage("KVSPACE_RUNTIME_MEMBER_SEP") != NULL ||
        kvspaceConstStorage("KVSPACE_ERR_INDEX_MUST_END_WITH_SEPARATOR") != NULL ||
        kvspaceConstStorage("KVSPACE_ERR_INVALID_PATH") != NULL) {
        fprintf(stderr, "FAIL export_surface: removed const still present\n");
        return 1;
    }
    printf("ok: %zu exported symbols linked, const query works\n",
           sizeof(exported_symbols) / sizeof(exported_symbols[0]));
    return 0;
}
