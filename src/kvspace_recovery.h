#ifndef KVSPACE_RECOVERY_H
#define KVSPACE_RECOVERY_H
#include "kvspace_internal.h"
int recovery_register_claim(kvspace_t *store);
void recovery_register_release(kvspace_t *store);
uint32_t recovery_fork_generation(void);
void recovery_mark_active(kvspace_t *store);
void recovery_ack_progress(kvspace_t *store);
int store_write_lock(kvspace_t *store);
void store_write_unlock(kvspace_t *store);
void recovery_repair_dead_writer(kvspace_t *store);
int recovery_maintenance_start(kvspace_t *store);
void recovery_maintenance_stop(kvspace_t *store);
bool recovery_claim_alive(kvspace_t *store, uint32_t claim);
bool recovery_other_alive(kvspace_t *store);
int recovery_round(kvspace_t *store);
#endif
