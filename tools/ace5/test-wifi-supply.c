/* SPDX-License-Identifier: BSD-2-Clause */
/* Production supply coordinator + leaf contract models. The real GPIO,
 * AOP/PDC, BCM/RPMh/rail and GCC MMIO cores have separate cloud regressions.
 * This fixture proves composition/failure lifetime, not hardware support. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "giulia_wifi_supply.h"
struct mock {
    struct giulia_wifi_supply *s;
    char trace[256], start_fail, stop_fail;
    unsigned count, external_calls, bad_external_at, persist_external;
    unsigned table_checks, rail_checks, cpu_plans, held_checks, bad_input;
    unsigned bad_held_at, modes, handoff, keep_pdc, keep_rails, contract_bad, cpu_resumes;
};
static struct mock *m;
static int event(char c)
{
    assert(m->count < sizeof(m->trace)-1); m->trace[m->count++]=c;
    if (m->start_fail==c) { m->start_fail=0; return -55; }
    if (m->stop_fail==c) { m->stop_fail=0; return -66; }
    return 0;
}
static void flags(uint32_t got,uint32_t need) { assert((got&need)==need); }
static int external(void *cookie,uint32_t required,unsigned stage)
{
    assert(cookie==m);
    assert(required==(GIULIA_SUPPLY_REQUIRED | (m->modes ? GIULIA_SUPPLY_DT_MODE_WRITE_SAFE : 0) |
        (m->handoff ? GIULIA_SUPPLY_RCHNG_HANDOFF : 0)));
    assert(stage>GIULIA_SUPPLY_NONE && stage<=GIULIA_SUPPLY_HOST_HANDOFF);
    ++m->external_calls;
    if (m->bad_external_at && (m->external_calls==m->bad_external_at ||
        (m->persist_external && m->external_calls>=m->bad_external_at))) return -91;
    return 0;
}
static uint32_t unused_read(void *p,uint32_t off) { (void)p;(void)off; assert(!"leaf MMIO belongs to its own fixture");return 0; }
static void unused_write(void *p,uint32_t off,uint32_t v) { (void)p;(void)off;(void)v;assert(!"unexpected leaf MMIO"); }
static void unused_barrier(void *p) { (void)p;assert(!"unexpected barrier"); }
static void unused_delay(void *p,unsigned us) { (void)p;(void)us;assert(!"unexpected delay"); }
static int unused_kick(void *p) { (void)p;assert(!"unexpected doorbell");return 0; }
static uint32_t unused_block_read(void *p,unsigned block,uint32_t off) { (void)block;return unused_read(p,off); }
static void unused_block_write(void *p,unsigned block,uint32_t off,uint32_t v) { (void)block;unused_write(p,off,v); }
static uint8_t table[508], db[1024];
static struct giulia_wlan_rail_baseline baseline[7];
static struct giulia_wlan_pdc_baseline old_table;
static struct giulia_wifi_supply_config setup(struct mock *m0,struct giulia_wifi_supply *s)
{
    m=m0; memset(m,0,sizeof(*m));memset(s,0,sizeof(*s));m->s=s;
    memset(table,0x50,sizeof(table));memset(db,0xdb,sizeof(db));
    memset(baseline,0,sizeof(baseline));old_table=(struct giulia_wlan_pdc_baseline){.known_mask=0x3ff};
    for(unsigned i=0;i<7;i++) { baseline[i].known=7;baseline[i].millivolts=100+i; }
    struct giulia_wifi_supply_config c={0};
    c.gpio=(struct giulia_wifi_gpio_io){m,unused_read,unused_write,unused_barrier,unused_delay};
    c.aop=(struct giulia_aop_io){m,unused_read,unused_write,unused_barrier,unused_delay,unused_kick};
    c.rpmh=(struct giulia_rpmh_io){m,unused_read,unused_write,unused_barrier,unused_delay};
    c.power=(struct giulia_pcie_power_io){m,unused_block_read,unused_block_write,unused_barrier,unused_delay};
    c.db=db;c.db_bytes=sizeof(db);c.db_version=7;c.pdc_table=table;c.pdc_bytes=508;
    c.rail_baseline=baseline;c.pdc_baseline=&old_table;c.conditions=GIULIA_SUPPLY_REQUIRED;
    c.aop_policy=GIULIA_AOP_OPEN_ON_INHERITED_LINK;c.pdc_policy=GIULIA_WLAN_PDC_RESTORE_KNOWN;
    c.allow_retained_cpu_floor=1;c.owner_cookie=m;c.validate_external=external;
    return c;
}
int giulia_wlan_pdc_check(const uint8_t *p,uint32_t bytes,const struct giulia_wlan_pdc_baseline *b,unsigned policy)
{ m->table_checks++;assert(p && bytes==508 && p[0]==0x50);assert(policy==m->keep_pdc);if(!policy)assert(b&&b->known_mask==0x3ff);return m->bad_input==1 ? -21 : 0; }
int giulia_wlan_rails_check(const uint8_t *p,uint32_t bytes,uint32_t version,const struct giulia_wlan_rail_baseline *b,uint32_t f,enum giulia_wlan_rail_policy policy,unsigned modes)
{ m->rail_checks++;assert(p==db && bytes==1024 && version==7 && b);flags(f,15);assert(policy==(m->keep_rails?GIULIA_WLAN_KEEP_VOTES:GIULIA_WLAN_RESTORE_BASELINE)&&modes==m->modes);return m->bad_input==2 ? -20 : 0; }
int giulia_cpu_noc_plan(const uint8_t *p,uint32_t bytes,uint32_t version,struct giulia_cpu_noc_plan *out)
{ m->cpu_plans++;assert(p==db&&bytes==1024&&version==7&&out);return m->bad_input==3 ? -21 : 0; }
int giulia_wifi_gpio_begin_with_bt_reset(struct giulia_wifi_gpio *g,uint32_t f,const struct giulia_wifi_gpio_io *io)
{ flags(f,GIULIA_WGPIO_BEGIN_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);assert(g==&m->s->gpio&&io->cookie==m);int rc=event('G');g->io=*io;g->bt_owned=1;g->dirty=31;g->state=rc?GIULIA_WGPIO_FAULT:GIULIA_WGPIO_HELD;return rc; }
int giulia_wifi_gpio_check_held(struct giulia_wifi_gpio *g,uint32_t f)
{ flags(f,GIULIA_WGPIO_BEGIN_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);assert(g==&m->s->gpio&&g->state==GIULIA_WGPIO_HELD&&g->bt_owned);return ++m->held_checks==m->bad_held_at ? GIULIA_WGPIO_BUSY : 0; }
int giulia_wifi_gpio_wlan_on(struct giulia_wifi_gpio *g,uint32_t f)
{ flags(f,GIULIA_WGPIO_ON_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);assert(m->s->rails.state==GIULIA_WLAN_RAIL_ACTIVE&&m->s->cpu_noc.completed);int rc=event('W');g->state=rc?GIULIA_WGPIO_FAULT:GIULIA_WGPIO_ON;return rc; }
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *g,uint32_t f)
{ flags(f,GIULIA_WGPIO_BEGIN_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);assert(g->bt_owned);return event('r'); }
int giulia_wifi_gpio_wlan_off_hold(struct giulia_wifi_gpio *g,uint32_t f)
{ flags(f,GIULIA_WGPIO_STOP_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);assert(m->s->power.state==GIULIA_POWER_IDLE||m->s->power.state==GIULIA_POWER_STOPPED);int rc=event('w');g->state=rc?GIULIA_WGPIO_FAULT:GIULIA_WGPIO_HELD;return rc; }
int giulia_wifi_gpio_stop(struct giulia_wifi_gpio *g,uint32_t f)
{ flags(f,GIULIA_WGPIO_STOP_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);assert(!m->s->retained&&g->state==GIULIA_WGPIO_HELD);int rc=event('g');g->state=rc?GIULIA_WGPIO_FAULT:GIULIA_WGPIO_STOPPED;if(!rc)g->bt_owned=g->dirty=0;return rc; }
int giulia_rpmh_init(struct giulia_rpmh *t,const struct giulia_rpmh_io *io,uint32_t f)
{ flags(f,GIULIA_RPMH_REQUIRED);int rc=event('I');t->io=*io;t->state=GIULIA_RPMH_IDLE;return rc; }
int giulia_aop_open(struct giulia_aop *a,const struct giulia_aop_io *io,uint32_t bytes,uint32_t f,unsigned policy)
{ flags(f,GIULIA_AOP_REQUIRED|GIULIA_AOP_INHERITED_CHANNEL_LEASE);assert(bytes==0x400&&policy==GIULIA_AOP_OPEN_ON_INHERITED_LINK&&m->s->gpio.state==GIULIA_WGPIO_HELD);int rc=event('A');a->io=*io;a->owned=1;a->state=rc?GIULIA_AOP_FAULT:GIULIA_AOP_OPEN;return rc; }
int giulia_aop_close(struct giulia_aop *a)
{ assert(!a->pending&&(m->s->pdc.state==GIULIA_WLAN_PDC_IDLE||m->s->pdc.state==GIULIA_WLAN_PDC_STOPPED||m->s->pdc.state==GIULIA_WLAN_PDC_HELD));int rc=event('a');if(!rc){a->owned=0;a->state=GIULIA_AOP_STOPPED;}return rc; }
int giulia_wlan_pdc_start(struct giulia_wlan_pdc *p,struct giulia_aop *a,const uint8_t *dt,uint32_t bytes,const struct giulia_wlan_pdc_baseline *b,uint32_t f,unsigned policy)
{ flags(f,GIULIA_WLAN_PDC_REQUIRED);assert(a==&m->s->aop&&a->state==GIULIA_AOP_OPEN&&dt==m->s->input_table&&bytes==508&&b==&m->s->input_pdc);int rc=event('D');p->aop=a;p->policy=policy;p->attempted=rc?1:0x3ff;p->delivered=rc?0:0x3ff;p->state=rc?GIULIA_WLAN_PDC_FAULT:GIULIA_WLAN_PDC_DELIVERED;if(rc)a->pending=1;return rc; }
int giulia_wlan_pdc_stop(struct giulia_wlan_pdc *p,uint32_t f)
{ flags(f,GIULIA_WLAN_PDC_REQUIRED);assert(m->s->gpio.state==GIULIA_WGPIO_HELD&&m->s->rails.state!=GIULIA_WLAN_RAIL_ACTIVE);int rc=event('d');if(rc)return rc;p->aop->pending=0;p->state=p->policy?GIULIA_WLAN_PDC_HELD:GIULIA_WLAN_PDC_STOPPED;return p->policy?GIULIA_WLAN_PDC_RETAINED:0; }
static int rails_on(struct giulia_wlan_rails *r,struct giulia_rpmh *t,const uint8_t *p,uint32_t bytes,uint32_t version,const struct giulia_wlan_rail_baseline *b,uint32_t f,enum giulia_wlan_rail_policy policy,unsigned modes)
{ flags(f,GIULIA_WLAN_RAIL_REQUIRED);assert(p==db&&bytes==1024&&version==7&&b==m->s->input_rails&&policy==(m->keep_rails?GIULIA_WLAN_KEEP_VOTES:GIULIA_WLAN_RESTORE_BASELINE)&&modes==m->modes);assert(m->s->pdc.state==GIULIA_WLAN_PDC_DELIVERED&&m->s->gpio.state==GIULIA_WGPIO_HELD);int rc=event('V');r->transport=t;r->policy=policy;r->state=rc?GIULIA_WLAN_RAIL_FAULT:GIULIA_WLAN_RAIL_ACTIVE;r->completed_mask=rc?0:0x7f;return rc; }
int giulia_wlan_rails_start(struct giulia_wlan_rails *r,struct giulia_rpmh *t,const uint8_t *p,uint32_t bytes,uint32_t version,const struct giulia_wlan_rail_baseline *b,uint32_t f,enum giulia_wlan_rail_policy policy)
{ return rails_on(r,t,p,bytes,version,b,f,policy,0); }
int giulia_wlan_rails_start_with_dt_modes(struct giulia_wlan_rails *r,struct giulia_rpmh *t,const uint8_t *p,uint32_t bytes,uint32_t version,const struct giulia_wlan_rail_baseline *b,uint32_t f,enum giulia_wlan_rail_policy policy)
{ flags(f,GIULIA_WLAN_RAIL_MODE_WRITE_SAFE);return rails_on(r,t,p,bytes,version,b,f,policy,1); }
int giulia_wlan_rails_stop(struct giulia_wlan_rails *r,int quiet)
{ assert(quiet&&m->s->gpio.state==GIULIA_WGPIO_HELD&&m->s->cpu_noc.state!=GIULIA_CPU_NOC_ACTIVE&&m->s->cpu_noc.state!=GIULIA_CPU_NOC_FAULT);int rc=event('v');if(rc){r->state=GIULIA_WLAN_RAIL_FAULT;return rc;}r->state=r->policy?GIULIA_WLAN_RAIL_HELD:GIULIA_WLAN_RAIL_STOPPED;return r->policy?GIULIA_WLAN_RAIL_RETAINED:0; }
int giulia_cpu_noc_start_keep(struct giulia_cpu_noc *p,struct giulia_rpmh *t,const struct giulia_cpu_noc_lease *lease,const uint8_t *db0,uint32_t bytes,uint32_t version,uint32_t f)
{ flags(f,GIULIA_CPU_NOC_REQUIRED);assert(db0==db&&bytes==1024&&version==7&&m->s->rails.state==GIULIA_WLAN_RAIL_ACTIVE);assert(!lease->validate(lease->cookie));int rc=event('C');p->transport=t;p->lease=*lease;p->triggered=1;p->completed=!rc;p->state=rc?GIULIA_CPU_NOC_FAULT:GIULIA_CPU_NOC_ACTIVE;return rc; }
int giulia_cpu_noc_stop_keep(struct giulia_cpu_noc *p)
{ assert(!p->lease.validate(p->lease.cookie));int rc=event('c');if(rc)return rc;p->completed=1;p->state=GIULIA_CPU_NOC_HELD;m->cpu_resumes++;return GIULIA_CPU_NOC_RETAINED; }
int giulia_pcie_power_prepare_host(struct giulia_pcie_power *p,const struct giulia_pcie_power_io *io,uint32_t f)
{ flags(f,GIULIA_POWER_HOST_REQUIRED|GIULIA_POWER_CESTA_COORDINATED);assert(!(f&(GIULIA_POWER_NOC_READY|GIULIA_POWER_DIRECT_VOTES)));assert(!!(f&GIULIA_POWER_RCHNG_HANDOFF)==!!m->handoff);assert(m->s->gpio.state==GIULIA_WGPIO_ON);int rc=event('P');p->io=*io;p->state=rc?GIULIA_POWER_FAULT:GIULIA_POWER_HOST_PREPARED;if(m->contract_bad)p->state=GIULIA_POWER_READY;return rc; }
int giulia_pcie_power_stop(struct giulia_pcie_power *p,uint32_t f)
{ flags(f,GIULIA_POWER_STOP_REQUIRED|GIULIA_POWER_STOP_CESTA_RESOURCES_QUIET);assert(!p->reset_attempted);int rc=event('p');p->state=rc?GIULIA_POWER_FAULT:GIULIA_POWER_STOPPED;return rc; }

int main(int argc,char **argv)
{
    struct mock mock;struct giulia_wifi_supply s,other={0};
    struct giulia_wlan_rail_baseline unk[7]={0};
    struct giulia_wifi_supply_config c=setup(&mock,&s);
    assert(!giulia_wifi_supply_cancel(&s));
    for(unsigned i=0;i<13;i++){if(!(GIULIA_SUPPLY_REQUIRED&(1u<<i)))continue;c.conditions=GIULIA_SUPPLY_REQUIRED&~(1u<<i);assert(giulia_wifi_supply_prepare(&s,&c)==GIULIA_SUPPLY_PREREQUISITE);}
    c.conditions=GIULIA_SUPPLY_REQUIRED;assert(!m->count&&!m->external_calls);
    for(unsigned i=1;i<4;i++){m->bad_input=i;assert(giulia_wifi_supply_prepare(&s,&c)==GIULIA_SUPPLY_DRIVER);assert(!m->count&&s.failed_stage==GIULIA_SUPPLY_INPUTS);}
    m->bad_input=0;
    if(argc>1&&!strcmp(argv[1],"--start-fail")){assert(argc==3);m->start_fail=argv[2][0];}
    if(argc>1&&!strcmp(argv[1],"--modes")){m->modes=m->handoff=1;c.dt_modes=1;c.conditions|=GIULIA_SUPPLY_DT_MODE_WRITE_SAFE|GIULIA_SUPPLY_RCHNG_HANDOFF;}
    if(argc>1&&!strcmp(argv[1],"--keep-pdc")){m->keep_pdc=1;c.pdc_policy=GIULIA_WLAN_PDC_KEEP_TABLE;}
    if(argc>1&&!strcmp(argv[1],"--keep-rails")){m->keep_rails=1;c.rail_baseline=unk;c.rail_policy=GIULIA_WLAN_KEEP_VOTES;}
    if(argc>1&&!strcmp(argv[1],"--contract"))m->contract_bad=1;
    if(argc>1&&!strcmp(argv[1],"--guard")){assert(argc==3);m->bad_held_at=(unsigned)atoi(argv[2]);}
    if(argc>1&&!strcmp(argv[1],"--early-stop-fail")){m->start_fail='I';m->stop_fail='g';}
    if(argc>1&&!strcmp(argv[1],"--external")){m->bad_external_at=m->external_calls+4;m->persist_external=1;}
    int rc=giulia_wifi_supply_prepare(&s,&c);
    if(rc){
        assert(rc==GIULIA_SUPPLY_DRIVER||rc==GIULIA_SUPPLY_EXTERNAL||rc==GIULIA_SUPPLY_CONTRACT);
        assert(s.failed_stage!=GIULIA_SUPPLY_NONE);
        if(m->contract_bad){assert(s.state==GIULIA_SUPPLY_FAULT&&s.power.state==GIULIA_POWER_READY&&s.cleanup_error==GIULIA_SUPPLY_STATE);assert(!strchr(m->trace,'p'));puts("Ace5 supply: unexpected later reset/owner cannot be cancelled as phase one");return 0;}
        if(m->persist_external){assert(s.state==GIULIA_SUPPLY_FAULT&&s.cleanup_error);m->persist_external=m->bad_external_at=0;rc=giulia_wifi_supply_cancel(&s);assert(rc==0||rc==GIULIA_SUPPLY_RETAINED);}
        else if(s.state==GIULIA_SUPPLY_FAULT){assert(s.cleanup_error);rc=giulia_wifi_supply_cancel(&s);assert(rc==0||rc==GIULIA_SUPPLY_RETAINED);}
        else assert(s.state==GIULIA_SUPPLY_STOPPED||s.state==GIULIA_SUPPLY_HELD);
        assert(!strchr(m->trace,'W')||strchr(m->trace,'w'));
    }else{
        assert(!strcmp(m->trace,"GIADVCWP")&&s.state==GIULIA_SUPPLY_HOST_PREPARED&&!s.power.reset_attempted);
        assert(giulia_wifi_supply_prepare(&other,&c)==GIULIA_SUPPLY_STATE);
        if(argc>1&&!strcmp(argv[1],"--handoff")){
            assert(!giulia_wifi_supply_handoff_host(&s));unsigned before=m->count;
            assert(giulia_wifi_supply_cancel(&s)==GIULIA_SUPPLY_STATE&&m->count==before);
            assert(s.state==GIULIA_SUPPLY_HANDED_OFF);puts("Ace5 supply: explicit host handoff blocks premature lower teardown");return 0;
        }
        if(argc>1&&!strcmp(argv[1],"--reset-attempted")){
            s.power.reset_attempted=1;s.power.state=GIULIA_POWER_FAULT;unsigned before=m->count;
            assert(giulia_wifi_supply_cancel(&s)==GIULIA_SUPPLY_STATE&&m->count==before);puts("Ace5 supply: unexpected later reset/owner cannot be cancelled as phase one");return 0;
        }
        if(argc>1&&!strcmp(argv[1],"--reset-stopped")){
            /* The board owner already stopped a later reset/PHY lifecycle.
             * Do not re-enter power_stop or reject its historical marker. */
            s.power.reset_attempted=1;s.power.state=GIULIA_POWER_STOPPED;
        }
        if(argc>1&&!strcmp(argv[1],"--stop-fail")){assert(argc==3);m->stop_fail=argv[2][0];}
        rc=giulia_wifi_supply_cancel(&s);
        if(rc<0){assert(s.state==GIULIA_SUPPLY_FAULT&&s.cleanup_stage!=GIULIA_SUPPLY_NONE);assert(giulia_wifi_supply_cancel(&s)==GIULIA_SUPPLY_RETAINED);}
        else assert(rc==GIULIA_SUPPLY_RETAINED);
        assert(s.state==GIULIA_SUPPLY_HELD&&s.gpio.state==GIULIA_WGPIO_HELD&&s.gpio.bt_owned);
        assert(s.power.state==GIULIA_POWER_STOPPED&&s.rails.state==(c.rail_policy?GIULIA_WLAN_RAIL_HELD:GIULIA_WLAN_RAIL_STOPPED)&&!s.aop.owned);
        assert(s.retained==(GIULIA_SUPPLY_RETAINS_CPU_NOC|(m->keep_pdc?GIULIA_SUPPLY_RETAINS_PDC:0)|(c.rail_policy?GIULIA_SUPPLY_RETAINS_RAILS:0)));
        unsigned before=m->count;assert(giulia_wifi_supply_cancel(&s)==GIULIA_SUPPLY_RETAINED&&m->count==before);
    }
    puts("Ace5 supply: actual coordinator ordering, preflight, reverse unwind and retained GPIO ownership passed");
    return 0;
}
