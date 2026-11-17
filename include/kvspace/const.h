#ifndef KVSPACE_CONST_H
#define KVSPACE_CONST_H 
#define KVSPACE_DIR_INDEX_SUF "/"
#define KVSPACE_MEMBER_SEP "·"
#define KVSPACE_KV(X) \
    X(KVSPACE_DIR_INDEX_SUF) \
    X(KVSPACE_MEMBER_SEP)
const char *kvspaceConstStorage(const char *name);
#endif
