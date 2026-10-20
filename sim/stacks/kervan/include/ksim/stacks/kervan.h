/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KSIM_STACKS_KERVAN_H
#define KSIM_STACKS_KERVAN_H

#include "ksim/stack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kervan core running as a simulator stack. Config keys: docs/simulator.md. */
const ksim_stack_t *ksim_stack_kervan(void);

#ifdef __cplusplus
}
#endif

#endif /* KSIM_STACKS_KERVAN_H */
