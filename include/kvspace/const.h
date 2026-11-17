#ifndef KVSPACE_CONST_H
#define KVSPACE_CONST_H 
#define KVSPACE_DIR_INDEX_SUF "/"
#define KVSPACE_MEMBER_SEP "·"
#define KVSPACE_ERR_INDEX_MUST_END_WITH_SEPARATOR "kvspace: index must end with /·"
#define KVSPACE_ERR_INVALID_PATH "kvspace: path must be absolute and canonical"
#define KVSPACE_KV(X) \
    X(KVSPACE_DIR_INDEX_SUF) \
    X(KVSPACE_MEMBER_SEP) \
    X(KVSPACE_ERR_INDEX_MUST_END_WITH_SEPARATOR) \
    X(KVSPACE_ERR_INVALID_PATH)
const char *kvspaceConstStorage(const char *name);
#endif
