/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_CESTA_H
#define GIULIA_PCIE_CESTA_H

#include <stdint.h>

#define GIULIA_CESTA_SEQUENCE_WORDS 13u
#define GIULIA_CESTA_BRANCH_WORDS 7u
#define GIULIA_CESTA_WORDS (GIULIA_CESTA_SEQUENCE_WORDS + GIULIA_CESTA_BRANCH_WORDS)
#define GIULIA_CESTA_BRANCH_OFFSET 0x1000u
#define GIULIA_CESTA_START_OFFSET 0x1090u

enum giulia_cesta_result {
    GIULIA_CESTA_OK, GIULIA_CESTA_RETAINED,
    GIULIA_CESTA_ARGUMENT = -1, GIULIA_CESTA_STATE = -2,
    GIULIA_CESTA_LEASE = -3, GIULIA_CESTA_IO = -4,
    GIULIA_CESTA_BUSY = -5, GIULIA_CESTA_DRIFT = -6
};

enum giulia_cesta_state {
    GIULIA_CESTA_IDLE, GIULIA_CESTA_PARTIAL, GIULIA_CESTA_STAGED,
    GIULIA_CESTA_RESET_REQUIRED
};

struct giulia_cesta_io {
    void *cookie;
    int (*read)(void *, uint32_t offset, uint32_t *value);
    int (*write)(void *, uint32_t offset, uint32_t value);
    void (*barrier)(void *);
    int (*validate_lease)(void *);
    int (*validate_reset)(void *);
};

struct giulia_cesta {
    struct giulia_cesta_io io;
    uint32_t image[GIULIA_CESTA_WORDS];
    uint32_t dirty;
    enum giulia_cesta_state state;
};

/* Offsets are relative to the verified giulia PCIe0 SM resource. A zero
 * lease result requires exclusive SM ownership, ordered accessible MMIO,
 * endpoint reset and no active PCIe users. Validate the exact board/vendor
 * image independently; this API validates layout, not instruction semantics.
 * Image arrays contain the DT property bytes in big-endian order. Begin with
 * a zero-initialized owner. If staging fails with state PARTIAL, retain the
 * mappings and lease until discard succeeds or the device is reset. */
int giulia_cesta_stage(struct giulia_cesta *, const struct giulia_cesta_io *,
    const uint8_t *sequence, uint32_t sequence_bytes, const uint8_t *branches,
    uint32_t branch_bytes, uint32_t branch_offset, uint32_t start_offset);

/* Zero validate_reset requires a verified independent reset, armed now
 * and able to return to Android without XNU or USB. A START attempt is
 * irreversible here even if the write callback fails. RETAINED means
 * reset required, not proof that the sequence executed. */
int giulia_cesta_start(struct giulia_cesta *);
int giulia_cesta_discard(struct giulia_cesta *);

#endif
