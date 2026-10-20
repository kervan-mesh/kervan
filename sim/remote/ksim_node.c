/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ksim-node: serve a built-in stack out of process.
 *
 *   ksim-node --stack kervan                 (port and node id from KSIM_PORT / KSIM_NODE_ID)
 *   ksim-node --stack nrf24-esb --port 47000 --node 4
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksim/remote.h"
#include "ksim/stacks/kervan.h"
#include "ksim/stacks/nrf24_esb.h"

static int usage(void)
{
    fprintf(stderr, "usage: ksim-node --stack kervan|nrf24-esb [--port P --node N]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const ksim_stack_t *stack = NULL;
    const char *port = NULL;
    const char *node = NULL;
    int i;

    for (i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--stack") == 0) {
            if (strcmp(argv[i + 1], "kervan") == 0) {
                stack = ksim_stack_kervan();
            } else if (strcmp(argv[i + 1], "nrf24-esb") == 0) {
                stack = ksim_stack_nrf24_esb();
            } else {
                fprintf(stderr, "ksim-node: unknown stack '%s'\n", argv[i + 1]);
                return 2;
            }
        } else if (strcmp(argv[i], "--port") == 0) {
            port = argv[i + 1];
        } else if (strcmp(argv[i], "--node") == 0) {
            node = argv[i + 1];
        } else {
            return usage();
        }
    }
    if (stack == NULL || i != argc || (port == NULL) != (node == NULL)) {
        return usage();
    }
    if (port != NULL) {
        return ksim_remote_serve(stack, (uint16_t)strtoul(port, NULL, 10),
                                 (uint32_t)strtoul(node, NULL, 10));
    }
    return ksim_remote_serve_env(stack);
}
