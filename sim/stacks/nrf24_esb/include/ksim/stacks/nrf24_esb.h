/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KSIM_STACKS_NRF24_ESB_H
#define KSIM_STACKS_NRF24_ESB_H

#include "ksim/stack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nRF24L01+ Enhanced ShockBurst link layer. Model and config keys: docs/nrf24-esb.md. */
const ksim_stack_t *ksim_stack_nrf24_esb(void);

#ifdef __cplusplus
}
#endif

#endif /* KSIM_STACKS_NRF24_ESB_H */
