/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_pcie_cesta.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
    uint32_t regs[0x1094 / 4];
    unsigned reads, writes, starts, failed_write;
    int lease_lost, reset_ready;
};

static int read_reg(void *cookie, uint32_t offset, uint32_t *value)
{
    struct fake *fake = cookie;
    assert(offset < sizeof(fake->regs) && !(offset & 3u));
    fake->reads++;
    *value = fake->regs[offset / 4];
    return 0;
}

static int write_reg(void *cookie, uint32_t offset, uint32_t value)
{
    struct fake *fake = cookie;
    assert(!fake->lease_lost && !fake->starts);
    assert((offset < 52u || (offset >= 0x1000u && offset < 0x101cu) ||
        offset == 0x1090u) && !(offset & 3u));
    if (offset == 0x1090u) {
        assert(fake->reset_ready && value == 1);
        fake->starts++;
    }
    fake->regs[offset / 4] = value;
    return ++fake->writes == fake->failed_write ? -1 : 0;
}

static int lease(void *cookie) { return ((struct fake *)cookie)->lease_lost; }
static int reset(void *cookie) { return !((struct fake *)cookie)->reset_ready; }
static void barrier(void *cookie) { (void)cookie; }

static int stage(struct giulia_cesta *sm, struct fake *fake, uint32_t sequence_bytes)
{
    uint8_t sequence[52] = {0};
    uint8_t branches[28] = {0};
    for (unsigned index = 0; index < sizeof(sequence); index++)
        sequence[index] = (uint8_t)(index + 1u);
    for (unsigned index = 0; index < 7u; index++)
        branches[4u * index + 3u] = (uint8_t)(4u * (index + 1u));
    const struct giulia_cesta_io io = {fake, read_reg, write_reg, barrier, lease, reset};
    return giulia_cesta_stage(sm, &io, sequence, sequence_bytes, branches,
        sizeof(branches), 0x1000u, 0x1090u);
}

int main(void)
{
    struct fake fake = {0};
    struct giulia_cesta sm = {0};
    assert(stage(&sm, &fake, 48) == GIULIA_CESTA_ARGUMENT && !fake.reads && !fake.writes);
    fake.regs[0x1048 / 4] = 1;
    assert(stage(&sm, &fake, 52) == GIULIA_CESTA_BUSY && !fake.writes);
    fake.regs[0x1048 / 4] = 0;
    fake.regs[0x1090 / 4] = 1;
    assert(stage(&sm, &fake, 52) == GIULIA_CESTA_BUSY && !fake.writes);
    fake.regs[0x1090 / 4] = 0;
    fake.regs[0x1000 / 4] = 4;
    assert(stage(&sm, &fake, 52) == GIULIA_CESTA_BUSY && !fake.writes);
    fake.regs[0x1000 / 4] = 0;
    assert(stage(&sm, &fake, 52) == 0 && fake.writes == 20 && !fake.starts);
    assert(fake.regs[0] == 0x01020304u && fake.regs[0x1000 / 4] == 4);
    assert(giulia_cesta_start(&sm) == GIULIA_CESTA_LEASE && fake.writes == 20);

    fake.regs[0x1018 / 4] = 0x20u;
    fake.reset_ready = 1;
    assert(giulia_cesta_start(&sm) == GIULIA_CESTA_DRIFT && fake.writes == 20);
    assert(giulia_cesta_discard(&sm) == GIULIA_CESTA_DRIFT && fake.writes == 20);
    fake.regs[0x1018 / 4] = 28;
    assert(giulia_cesta_discard(&sm) == 0 && fake.writes == 40 && !fake.starts);
    const uint32_t blank[sizeof(fake.regs) / 4] = {0};
    assert(!memcmp(fake.regs, blank, sizeof(blank)));

    for (unsigned index = 1; index <= 20; index++) {
        memset(&fake, 0, sizeof(fake));
        fake.failed_write = index;
        assert(stage(&sm, &fake, 52) == GIULIA_CESTA_IO && !fake.starts);
        assert(sm.state == GIULIA_CESTA_PARTIAL && sm.dirty);
        fake.lease_lost = 1;
        assert(giulia_cesta_discard(&sm) == GIULIA_CESTA_LEASE && fake.writes == index);
        fake.lease_lost = 0;
        fake.failed_write = index + 1;
        assert(giulia_cesta_discard(&sm) == GIULIA_CESTA_IO && sm.dirty);
        assert(giulia_cesta_discard(&sm) == 0 && !sm.dirty);
        assert(!memcmp(fake.regs, blank, sizeof(blank)));
    }

    for (unsigned failure = 0; failure < 2; failure++) {
        memset(&fake, 0, sizeof(fake));
        memset(&sm, 0, sizeof(sm));
        assert(stage(&sm, &fake, 52) == 0);
        fake.reset_ready = 1;
        fake.failed_write = failure ? fake.writes + 1 : 0;
        assert(giulia_cesta_start(&sm) == (failure ? GIULIA_CESTA_IO : GIULIA_CESTA_RETAINED));
        assert(sm.state == GIULIA_CESTA_RESET_REQUIRED && fake.starts == 1);
        assert(giulia_cesta_start(&sm) == GIULIA_CESTA_STATE);
        assert(giulia_cesta_discard(&sm) == GIULIA_CESTA_RETAINED);
        assert(fake.writes == 21 && fake.regs[0] == 0x01020304u);
    }
    puts("Ace5 CESTA: DT layout, cold staging, partial rollback, reset-gated START and retained ownership passed");
    return 0;
}
