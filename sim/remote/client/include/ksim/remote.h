/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KSIM_REMOTE_H
#define KSIM_REMOTE_H

#include <stdint.h>

#include "ksim/stack.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run an unmodified ksim stack out of process (docs/remote-protocol.md): connect to ksim on
 * 127.0.0.1:port as `node_id`, then serve events in lockstep until ksim ends the run.
 * Returns 0 after a clean end, non-zero on errors (described on stderr).
 */
int ksim_remote_serve(const ksim_stack_t *stack, uint16_t port, uint32_t node_id);

/* Same, with port and node id taken from KSIM_PORT and KSIM_NODE_ID. */
int ksim_remote_serve_env(const ksim_stack_t *stack);

#ifdef __cplusplus
}
#endif

#endif /* KSIM_REMOTE_H */
