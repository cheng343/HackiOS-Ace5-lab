/* SPDX-License-Identifier: BSD-2-Clause */
/* Reuse the RSC peripheral fixture; link the real CPU-NoC/BCM/RPMh cores.
 * Synthetic CMD DB version7, resources0x51000/4 are not hardware constants. */
#define GIULIA_RPMH_CPU_NOC_TEST
#define main original_rpmh_test_main
#include "test-rpmh.c"
#undef main
#include "giulia_pcie_cpu_noc.h"
static unsigned lease_calls, fail_lease_at;
static int lease_check(void *cookie)
{
    assert(cookie == &lease_calls);
    return ++lease_calls == fail_lease_at ? -1 : 0;
}
static void cpu_database(uint8_t db[1024])
{
    memset(db,0,1024); le32(db,7); le32(db+4,0x0c0330db);
    le16(db+8,5); le16(db+12,128); le16(db+14,2);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *e = db + 144 + 24*i, *a = db + 272 + 8*i;
        memcpy(e,i ? "CN0" : "SH1",3); le32(e+16,0x51000 + 4*i);
        le16(e+20,8); le16(e+22,8*i);
        le32(a,i ? 3 : 17000); le16(a+4,i ? 9 : 4); a[6]=(uint8_t)(i ? 5 : 1);
    }
}
static void planning(void)
{
    struct giulia_cpu_noc_plan p;
    uint8_t db[1024]; cpu_database(db);
    assert(!giulia_cpu_noc_plan(db,sizeof(db),7,&p));
    assert(p.command[0].address == 0x51000 && p.command[1].address == 0x51004);
    for (unsigned i = 0; i < 2; i++) {
        assert(!p.vote[i].x && p.vote[i].y == 1 && !p.vote[i].saturated);
        assert(p.vote[i].command == 0x60000001u && p.command[i].data == 0x60000001u);
    }
    db[272+6]=7; db[272+8+6]=2;
    assert(!giulia_cpu_noc_plan(db,sizeof(db),7,&p));
    assert(p.command[0].address == 0x51004 && p.command[1].address == 0x51000);
    db[272+6]=2;
    assert(!giulia_cpu_noc_plan(db,sizeof(db),7,&p));
    assert(p.command[0].data == 0x60000001u && p.command[1].data == 0x60000001u);
    cpu_database(db); le32(db+144+24+16,0x51000);
    assert(giulia_cpu_noc_plan(db,sizeof(db),7,&p) == GIULIA_BCM_DATABASE && !p.command[0].address);
    cpu_database(db); memcpy(db+144+24,"BAD",3);
    assert(giulia_cpu_noc_plan(db,sizeof(db),7,&p) == GIULIA_BCM_NOT_FOUND);
}
int main(int argc, char **argv)
{
    struct mock m; struct giulia_rpmh t; struct giulia_cpu_noc p = {0}, other = {0};
    struct giulia_cpu_noc_lease lease = {&lease_calls,lease_check};
    uint8_t db[1024]; planning(); cpu_database(db);
    for (unsigned i = 0; i < 5; i++) {
        setup(&m,&t);
        assert(giulia_cpu_noc_start_keep(&p,&t,&lease,db,sizeof(db),7,
            GIULIA_CPU_NOC_REQUIRED & ~BIT(i)) == GIULIA_RPMH_PREREQUISITE);
        assert(!m.reads && !m.writes && !lease_calls);
    }
    setup(&m,&t); fail_lease_at = 1;
    assert(giulia_cpu_noc_start_keep(&p,&t,&lease,db,sizeof(db),7,31) == GIULIA_RPMH_PREREQUISITE);
    assert(!m.reads && !m.writes);
    lease_calls = 0; fail_lease_at = 2;
    assert(giulia_cpu_noc_start_keep(&p,&t,&lease,db,sizeof(db),7,31) == GIULIA_RPMH_PREREQUISITE);
    assert(p.state == GIULIA_CPU_NOC_STOPPED && !m.reads && !m.writes);
    lease_calls = fail_lease_at = 0;
    unsigned scenario = argc > 1 && !strcmp(argv[1],"--timeout") ? 1 :
        argc > 1 && !strcmp(argv[1],"--cleanup") ? 2 :
        argc > 1 && !strcmp(argv[1],"--pretrigger") ? 3 : 0;
    if (scenario == 1) m.hold_at = 1;
    if (scenario == 2) m.cleanup_block = 1;
    if (scenario == 3) m.drop = MSG;
    int rc = giulia_cpu_noc_start_keep(&p,&t,&lease,db,sizeof(db),7,31);
    if (scenario == 3) {
        assert(rc == GIULIA_RPMH_READBACK && !m.triggers && p.state == GIULIA_CPU_NOC_STOPPED);
        m.drop = 0;
        rc = giulia_cpu_noc_start_keep(&p,&t,&lease,db,sizeof(db),7,31);
        assert(!rc);
    } else if (scenario) {
        assert(rc == (scenario == 1 ? GIULIA_RPMH_TIMEOUT : GIULIA_RPMH_READBACK));
        assert(p.state == GIULIA_CPU_NOC_FAULT && giulia_rpmh_bus_owned(&t));
        assert(p.triggered && p.completed == (scenario == 2));
        assert(giulia_cpu_noc_start_keep(&p,&t,&lease,db,sizeof(db),7,31) == GIULIA_RPMH_STATE);
        assert(giulia_cpu_noc_stop_keep(&p) == rc && m.triggers == 1);
        if (scenario == 1) assert(m.cpu_vote[0] && !m.cpu_vote[1]);
        m.hold_at = m.cleanup_block = 0;
    } else {
        assert(!rc && p.state == GIULIA_CPU_NOC_ACTIVE && p.completed && p.triggered);
        restored(&m,&t);
        unsigned before = m.writes;
        assert(giulia_cpu_noc_start_keep(&other,&t,&lease,db,sizeof(db),7,31) == GIULIA_RPMH_BUSY);
        assert(m.writes == before);
        fail_lease_at = lease_calls + 1;
        assert(giulia_cpu_noc_stop_keep(&p) == GIULIA_RPMH_PREREQUISITE && m.writes == before);
        fail_lease_at = 0;
    }
    assert(giulia_cpu_noc_stop_keep(&p) == GIULIA_CPU_NOC_RETAINED);
    assert(p.state == GIULIA_CPU_NOC_HELD && p.completed && m.triggers == 1);
    restored(&m,&t);
    assert(m.cpu_vote[0] == 0x60000001u && m.cpu_vote[1] == 0x60000001u);
    unsigned before = m.writes;
    assert(giulia_cpu_noc_stop_keep(&p) == GIULIA_CPU_NOC_RETAINED && m.writes == before);
    assert(giulia_cpu_noc_start_keep(&other,&t,&lease,db,sizeof(db),7,31) == GIULIA_RPMH_BUSY);
    assert(m.triggers == 1);
    puts("Ace5 CPU NoC: real SH1/CN0 plan and RPMh floor, no off vote, no resend, retained ownership passed");
    return 0;
}
