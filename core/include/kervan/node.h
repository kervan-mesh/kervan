/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KERVAN_NODE_H
#define KERVAN_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kervan/frame.h"
#include "kervan/pal.h"
#include "kervan/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A Kervan node: publish/subscribe with QoS 0/1, fragmentation, a duty-cycle airtime budget and
 * multi-hop relaying (managed flooding). See docs/frame-format.md and docs/messaging.md.
 */

/* Compile-time limits; override with -D to trade RAM for capacity. */
#ifndef KV_MTU_MAX
#define KV_MTU_MAX 255u /* largest frame of any supported radio */
#endif
#ifndef KV_MAX_MSG_SIZE
#define KV_MAX_MSG_SIZE 256u /* topic id (2) + payload, after reassembly */
#endif
#ifndef KV_TX_QUEUE
#define KV_TX_QUEUE 4u /* own messages waiting to be sent or acknowledged */
#endif
#ifndef KV_ACK_QUEUE
#define KV_ACK_QUEUE 4u /* acknowledgements waiting for the radio */
#endif
#ifndef KV_RELAY_QUEUE
#define KV_RELAY_QUEUE 4u /* frames waiting to be relayed */
#endif
#ifndef KV_REASSEMBLY_SLOTS
#define KV_REASSEMBLY_SLOTS 2u /* fragmented messages received in parallel */
#endif
#ifndef KV_REASSEMBLY_TIMEOUT_MS
#define KV_REASSEMBLY_TIMEOUT_MS 30000u /* since the last fragment (duty cycles pace them) */
#endif
#ifndef KV_MAX_SUBSCRIPTIONS
#define KV_MAX_SUBSCRIPTIONS 8u
#endif
#ifndef KV_DEDUP_SIZE
#define KV_DEDUP_SIZE 32u /* remembered (src, seq, fragment) triples */
#endif
#ifndef KV_DEDUP_WINDOW_MS
#define KV_DEDUP_WINDOW_MS 30000u /* how long a triple counts as a duplicate */
#endif

#define KV_QOS0 0u /* at most once */
#define KV_QOS1 1u /* at least once: acknowledged, retried with backoff */

/* A delivered message. data/meta are valid only during the callback. */
typedef struct {
    uint16_t src;
    uint16_t dst; /* this node or KV_ADDR_BROADCAST */
    uint16_t topic;
    uint8_t seq;
    uint8_t hops; /* relays it passed */
    const uint8_t *data;
    size_t len;
    const kv_rx_meta_t *meta; /* of the last frame; NULL if the port has none */
} kv_message_t;

/* Unicast messages to this node are always delivered; broadcasts only for subscribed topics. */
typedef void (*kv_message_cb_t)(void *user, const kv_message_t *msg);

/* Outcome of a QoS 1 publish: KV_OK when acknowledged, KV_ERR_TIMEOUT after all retries. */
typedef void (*kv_ack_cb_t)(void *user, uint8_t seq, kv_status_t result);

typedef struct {
    uint16_t addr;
    uint8_t net_id;
    uint8_t hop_limit;           /* for messages this node originates */
    uint32_t ack_timeout_ms;     /* first wait for an ACK; doubles with each retry */
    uint8_t max_retries;         /* retransmissions after the first attempt */
    uint16_t duty_cycle_permille; /* airtime budget: 10 = 1 % (EU868); 0 = unlimited */
    uint32_t duty_burst_ms;      /* airtime credit that may build up while idle */
    uint32_t ack_debt_ms;        /* ACKs (own and relayed) may overdraw the budget this far */
    bool relay;                  /* take part in multi-hop forwarding */
    uint32_t relay_window_ms;    /* relays wait 0..window (weaker signal = sooner) + jitter */
    kv_message_cb_t on_message;  /* optional */
    kv_ack_cb_t on_ack;          /* optional */
    void *user;
} kv_node_config_t;

/* Counters for diagnostics and the simulator. */
typedef struct {
    uint32_t tx_frames;
    uint32_t tx_airtime_ms;
    uint32_t rx_frames;
    uint32_t rx_duplicates;
    uint32_t delivered;
    uint32_t acks_sent;
    uint32_t retransmissions;
    uint32_t relayed;
    uint32_t relay_cancelled;      /* another node relayed it first */
    uint32_t relay_dropped;        /* queue full or no airtime budget */
    uint32_t reassembly_dropped;   /* no slot, inconsistent or timed out */
    uint32_t budget_waits;         /* times a message had to wait for airtime budget */
} kv_node_stats_t;

/* ---- internal state; allocate kv_node_t yourself and treat its fields as private ---- */

typedef struct {
    uint8_t seq;
    uint8_t qos;
    uint8_t frag_count;
    uint8_t next_frag;
    uint8_t attempts; /* complete transmissions so far */
    bool awaiting_ack;
    bool budget_wait_counted;
    uint16_t dst;
    uint16_t len;
    uint32_t ack_deadline;
    uint8_t data[KV_MAX_MSG_SIZE]; /* topic id + payload */
} kv_tx_entry_t;

typedef struct {
    uint16_t dst;
    uint8_t acked_seq;
} kv_ack_entry_t;

typedef struct {
    bool used;
    bool is_ack;
    uint8_t seq;
    uint8_t frag_key;
    uint8_t hop_count;
    uint16_t src;
    uint16_t len;
    uint32_t due;
    uint8_t frame[KV_MTU_MAX];
} kv_relay_entry_t;

typedef struct {
    bool used;
    bool ack_req;
    uint8_t seq;
    uint8_t last;
    uint8_t hops;
    uint16_t src;
    uint16_t dst;
    uint16_t have; /* bitmap of received fragments */
    uint16_t len;
    uint32_t updated; /* last fragment arrival: the timeout counts from here */
    uint8_t data[KV_MAX_MSG_SIZE];
} kv_reassembly_t;

typedef struct {
    bool used;
    uint8_t seq;
    uint8_t frag_key;
    uint16_t src;
    uint32_t time_ms;
} kv_dedup_entry_t;

typedef struct {
    bool used;
    uint8_t seq;
    uint16_t src;
    uint32_t time_ms;
} kv_ack_record_t;

#define KV_ACK_RECORDS 8u

typedef enum { KV_TX_NONE, KV_TX_OWN, KV_TX_ACK, KV_TX_RELAY } kv_tx_kind_t;

typedef struct {
    kv_pal_t pal;
    kv_node_config_t cfg;
    kv_node_stats_t stats;
    uint8_t next_seq;

    uint8_t depth; /* > 0 while the node itself is running (callbacks must not re-enter) */

    bool tx_busy;
    kv_tx_kind_t tx_kind;
    uint8_t tx_own_seq; /* seq of the own message being transmitted */
    uint32_t tx_started;
    uint8_t tx_buf[KV_MTU_MAX];

    int32_t credit;       /* airtime budget in 1/1000 ms; may go negative after an ACK */
    uint32_t credit_time; /* when credit was last refilled */

    kv_tx_entry_t txq[KV_TX_QUEUE]; /* ring buffer, head is in flight */
    uint8_t txq_head;
    uint8_t txq_count;
    kv_ack_entry_t ackq[KV_ACK_QUEUE];
    uint8_t ackq_count;
    kv_ack_record_t acked[KV_ACK_RECORDS]; /* recently acknowledged messages */
    uint8_t acked_next;
    kv_relay_entry_t relays[KV_RELAY_QUEUE];
    kv_reassembly_t reasm[KV_REASSEMBLY_SLOTS];
    kv_dedup_entry_t dedup[KV_DEDUP_SIZE];
    uint8_t dedup_next;
    uint16_t subs[KV_MAX_SUBSCRIPTIONS];
    uint8_t subs_count;
} kv_node_t;

/* Defaults: hop limit 3, ACK timeout 5 s, 3 retries, 1 % duty cycle, relaying on with a 1 s
 * window. addr must still be set. */
void kv_node_config_default(kv_node_config_t *cfg);

kv_status_t kv_node_init(kv_node_t *node, const kv_pal_t *pal, const kv_node_config_t *cfg);

/* Receive broadcasts on a topic (unicasts are always received). */
kv_status_t kv_node_subscribe(kv_node_t *node, uint16_t topic);
kv_status_t kv_node_unsubscribe(kv_node_t *node, uint16_t topic);

/*
 * Queue a message for `dst` (or KV_ADDR_BROADCAST). Messages larger than one frame are fragmented
 * (all nodes of a network must use the same MTU). QoS 1 needs a unicast destination; on_ack
 * reports the outcome. Returns KV_ERR_BUSY when the queue is full, KV_ERR_NO_SPACE when the
 * message is too large. out_seq (may be NULL) receives the message's sequence number.
 */
kv_status_t kv_node_publish(kv_node_t *node, uint16_t dst, uint16_t topic, const void *data,
                            size_t len, uint8_t qos, uint8_t *out_seq);

/* Port -> core events. Call from the main loop, never from an interrupt. */
void kv_node_on_rx(kv_node_t *node, const uint8_t *buf, size_t len, const kv_rx_meta_t *meta);
void kv_node_on_tx_done(kv_node_t *node);
void kv_node_tick(kv_node_t *node);

/*
 * When kv_node_tick() should be called next. Returns false if nothing is scheduled.
 * Re-query after every call into the node.
 */
bool kv_node_next_deadline(const kv_node_t *node, uint32_t *at_ms);

const kv_node_stats_t *kv_node_stats(const kv_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* KERVAN_NODE_H */
