#ifndef ZIPC_IDENTITY_TEST_H
#define ZIPC_IDENTITY_TEST_H

#include <zipc/zipc.h>

void zipc_test_identity_reset(zipc_component_id_t component,
                              uint32_t session, uint32_t sequence);
void zipc_test_identity_random(
    zipc_status_t (*random_fn)(void *, size_t));
void zipc_test_identity_reserved(void (*reserved_fn)(void));
void zipc_test_protection(
    zipc_status_t (*protect_fn)(zipc_pool_t *, zipc_slot_id_t, bool));
void zipc_test_identity_snapshot(zipc_component_id_t component,
                                 uint32_t *session_state,
                                 uint32_t *sequence,
                                 uint32_t *active);
zipc_status_t zipc_test_identity_generate(zipc_component_id_t component,
                                           zipc_buffer_id_t *id_out);

#endif
