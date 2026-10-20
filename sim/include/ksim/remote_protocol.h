/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KSIM_REMOTE_PROTOCOL_H
#define KSIM_REMOTE_PROTOCOL_H

/* ksim remote protocol: message types and limits. Spec: docs/remote-protocol.md. */

#define KSIM_REMOTE_PROTOCOL_VERSION 1u
#define KSIM_REMOTE_MAX_MESSAGE 65536u /* bytes after the length prefix */

#define KSIM_REMOTE_MODE_LOCKSTEP 0u

#define KSIM_MSG_HELLO 0x01u
#define KSIM_MSG_ERROR 0x02u

#define KSIM_EV_CREATE 0x10u
#define KSIM_EV_RX 0x11u
#define KSIM_EV_TX_DONE 0x12u
#define KSIM_EV_TIMER 0x13u
#define KSIM_EV_DESTROY 0x14u

#define KSIM_REQ_RADIO_SEND 0x20u
#define KSIM_REQ_TIMER_SET 0x21u
#define KSIM_REQ_TIMER_CANCEL 0x22u
#define KSIM_REQ_TRACE 0x23u
#define KSIM_REQ_RANDOM 0x24u
#define KSIM_REQ_RADIO_GET 0x25u
#define KSIM_REQ_RADIO_SET 0x26u

#define KSIM_MSG_DONE 0x2Fu
#define KSIM_MSG_RESULT 0x30u

/* Environment variables a stack process reads */
#define KSIM_ENV_PORT "KSIM_PORT"
#define KSIM_ENV_NODE_ID "KSIM_NODE_ID"

#endif /* KSIM_REMOTE_PROTOCOL_H */
