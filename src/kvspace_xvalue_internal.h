#ifndef KVSPACE_XVALUE_INTERNAL_H
#define KVSPACE_XVALUE_INTERNAL_H
#include "kvspace_internal.h"
void storage_write_head(uint8_t *destination, uint64_t body_length, uint64_t attribute_length);
uint64_t store_slots_box_alloc(kvspace_t *store, size_t size);
void store_slots_box_free(kvspace_t *store, uint64_t offset);
uint64_t retirement_threshold(void);
int retirement_reserve(kvspace_t *store, uint64_t *index);
void retirement_commit(kvspace_t *store, uint64_t index, uint64_t offset, uint64_t bytes_retired);
void retirement_cancel(kvspace_t *store, uint64_t index);
int retirement_drain(kvspace_t *store, uint32_t epoch);
void retirement_release_all(kvspace_t *store);
#endif
