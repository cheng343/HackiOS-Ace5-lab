/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_pcie_cesta.h"

static uint32_t
decode_cell(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) | bytes[3];
}

static uint32_t
word_offset(unsigned index)
{
    if (index < GIULIA_CESTA_SEQUENCE_WORDS) return 4u * index;
    return GIULIA_CESTA_BRANCH_OFFSET + 4u * (index - GIULIA_CESTA_SEQUENCE_WORDS);
}

static int
read_word(const struct giulia_cesta_io *io, uint32_t offset, uint32_t *value)
{
    if (io->validate_lease(io->cookie)) return GIULIA_CESTA_LEASE;
    if (io->read(io->cookie, offset, value) || *value == UINT32_MAX) return GIULIA_CESTA_IO;
    return GIULIA_CESTA_OK;
}

static int
idle(const struct giulia_cesta_io *io)
{
    const uint32_t offsets[] = {GIULIA_CESTA_START_OFFSET, 0x1040u, 0x1048u};
    for (unsigned index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++) {
        uint32_t value;
        int rc = read_word(io, offsets[index], &value);
        if (rc) return rc;
        if (value != 0) return GIULIA_CESTA_BUSY;
    }
    return GIULIA_CESTA_OK;
}

static int
write_word(struct giulia_cesta *sm, uint32_t offset, uint32_t value)
{
    const struct giulia_cesta_io *io = &sm->io;
    int rc = io->write(io->cookie, offset, value);
    io->barrier(io->cookie);
    return rc ? GIULIA_CESTA_IO : GIULIA_CESTA_OK;
}

int
giulia_cesta_stage(struct giulia_cesta *sm, const struct giulia_cesta_io *io,
    const uint8_t *sequence, uint32_t sequence_bytes, const uint8_t *branches,
    uint32_t branch_bytes, uint32_t branch_offset, uint32_t start_offset)
{
    struct giulia_cesta next = {0};
    if (!sm || !io || !io->read || !io->write || !io->barrier ||
        !io->validate_lease || !sequence || !branches ||
        sequence_bytes != 4u * GIULIA_CESTA_SEQUENCE_WORDS ||
        branch_bytes != 4u * GIULIA_CESTA_BRANCH_WORDS ||
        branch_offset != GIULIA_CESTA_BRANCH_OFFSET ||
        start_offset != GIULIA_CESTA_START_OFFSET) return GIULIA_CESTA_ARGUMENT;
    if (sm->state != GIULIA_CESTA_IDLE || sm->dirty) return GIULIA_CESTA_STATE;
    next.io = *io;
    for (unsigned index = 0; index < GIULIA_CESTA_SEQUENCE_WORDS; index++) {
        next.image[index] = decode_cell(sequence + 4u * index);
        if (next.image[index] == UINT32_MAX) return GIULIA_CESTA_ARGUMENT;
    }
    for (unsigned index = 0; index < GIULIA_CESTA_BRANCH_WORDS; index++) {
        uint32_t address = decode_cell(branches + 4u * index);
        if ((address & 3u) || address >= sequence_bytes) return GIULIA_CESTA_ARGUMENT;
        next.image[GIULIA_CESTA_SEQUENCE_WORDS + index] = address;
    }
    if (!next.image[0] || !next.image[GIULIA_CESTA_SEQUENCE_WORDS])
        return GIULIA_CESTA_ARGUMENT;
    int rc = idle(io);
    if (rc) return rc;
    for (unsigned index = 0; index < GIULIA_CESTA_WORDS; index++) {
        uint32_t value;
        rc = read_word(io, word_offset(index), &value);
        if (rc) return rc;
        if (value != 0) return GIULIA_CESTA_BUSY;
    }
    *sm = next;
    sm->state = GIULIA_CESTA_PARTIAL;
    for (unsigned index = 0; index < GIULIA_CESTA_WORDS; index++) {
        uint32_t value;
        uint32_t offset = word_offset(index);
        if (!sm->image[index]) continue;
        if ((rc = idle(&sm->io)) || (rc = read_word(&sm->io, offset, &value))) return rc;
        if (value != 0) return GIULIA_CESTA_DRIFT;
        sm->dirty |= 1u << index;
        if ((rc = write_word(sm, offset, sm->image[index])) ||
            (rc = read_word(&sm->io, offset, &value))) return rc;
        if (value != sm->image[index]) return GIULIA_CESTA_DRIFT;
    }
    sm->state = GIULIA_CESTA_STAGED;
    return GIULIA_CESTA_OK;
}

int
giulia_cesta_start(struct giulia_cesta *sm)
{
    if (!sm) return GIULIA_CESTA_ARGUMENT;
    if (sm->state != GIULIA_CESTA_STAGED) return GIULIA_CESTA_STATE;
    if (!sm->io.validate_reset) return GIULIA_CESTA_LEASE;
    for (unsigned index = 0; index < GIULIA_CESTA_WORDS; index++) {
        uint32_t value;
        int rc = read_word(&sm->io, word_offset(index), &value);
        if (rc) return rc;
        if (value != sm->image[index]) return GIULIA_CESTA_DRIFT;
    }
    int rc = idle(&sm->io);
    if (rc) return rc;
    if (sm->io.validate_reset(sm->io.cookie) || sm->io.validate_lease(sm->io.cookie))
        return GIULIA_CESTA_LEASE;
    sm->state = GIULIA_CESTA_RESET_REQUIRED;
    rc = write_word(sm, GIULIA_CESTA_START_OFFSET, 1u);
    return rc ? rc : GIULIA_CESTA_RETAINED;
}

int
giulia_cesta_discard(struct giulia_cesta *sm)
{
    if (!sm) return GIULIA_CESTA_ARGUMENT;
    if (sm->state == GIULIA_CESTA_IDLE) return GIULIA_CESTA_OK;
    if (sm->state == GIULIA_CESTA_RESET_REQUIRED) return GIULIA_CESTA_RETAINED;
    if (sm->state != GIULIA_CESTA_PARTIAL && sm->state != GIULIA_CESTA_STAGED)
        return GIULIA_CESTA_STATE;
    sm->state = GIULIA_CESTA_PARTIAL;
    for (unsigned count = GIULIA_CESTA_WORDS; count > 0; count--) {
        unsigned index = count - 1u;
        uint32_t bit = 1u << index;
        if (!(sm->dirty & bit)) continue;
        uint32_t offset = word_offset(index), value;
        int rc = idle(&sm->io);
        if (rc) return rc;
        rc = read_word(&sm->io, offset, &value);
        if (rc) return rc;
        if (value && value != sm->image[index]) return GIULIA_CESTA_DRIFT;
        if (value) {
            if ((rc = write_word(sm, offset, 0)) ||
                (rc = read_word(&sm->io, offset, &value))) return rc;
            if (value) return GIULIA_CESTA_DRIFT;
        }
        sm->dirty &= ~bit;
    }
    sm->state = GIULIA_CESTA_IDLE;
    return GIULIA_CESTA_OK;
}
