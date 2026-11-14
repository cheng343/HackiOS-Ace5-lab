/* Actual user-client validation/recovery block connected to the real A750
 * backend. Its MMIO and allocator are inert host stubs. */
#ifdef NDEBUG
#error "Compile this test with -UNDEBUG"
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define NSEC_PER_USEC 1000
struct a750_memory { unsigned unused; };
struct a750_gx { struct a750_memory memory; };
struct a750_hfi { struct a750_gx *gx;vm_offset_t smmu,gmu; };
static uint32_t regs[0x4000],ring[8192],mem[16];
static bool a750_runtime_active;
uint32_t gpu_log,gpu_va;
static uint64_t mach_absolute_time(void) { return 0; }
static uint64_t *a750_leaf(struct a750_memory *m,uint32_t va,bool create)
{ (void)m;(void)va;(void)create;return NULL; }
static void a750_flush(vm_offset_t p,vm_size_t n) { (void)p;(void)n; }
static uint32_t a750_rd(vm_offset_t b,uint32_t o) { return b==1?regs[o/4]:0; }
static void a750_wr(vm_offset_t b,uint32_t o,uint32_t v) { if (b==1) regs[o/4]=v; }
static bool a750_poll(vm_offset_t b,uint32_t o,uint32_t m,uint32_t v)
{ (void)b;(void)o;(void)m;(void)v;return true; }
static bool a750_gx_faulted(struct a750_hfi *s) { (void)s;return false; }
static bool a750_gpu_idle(struct a750_hfi *s,vm_offset_t gx) { (void)s;(void)gx;return true; }
static int a750_log(const char *format,...) { (void)format;return 0; }
static void delay_for_interval(unsigned t,unsigned n) { (void)t;(void)n;assert(false); }
static void panic(const char *message) { fprintf(stderr,"%s\n",message);abort(); }
#include "../../xnu/files/bsd/sys/redwood_gpu_backend.h"
#include "../../xnu/files/bsd/dev/giulia_gpu_submit.h"
#include "../../xnu/files/bsd/dev/giulia_gpu_live.h"
static void a750_live_stop(void) { assert(false); }

/* Generated directly from RedwoodGPU.cpp, including the actual public structs. */
#include "gpu-submit-types.inc"
struct gpu_buffer { uint64_t address,size; };
static struct gpu_buffer test_buffer={0x70000000,0x800000};
static struct gpu_buffer *buffer(uint32_t handle) { return handle==1?&test_buffer:NULL; }
static unsigned backend_submits,reports,recoveries,unlocks;
static bool gGPUReported;
static int gGPULock;
static void IOLockUnlock(int lock) { (void)lock;unlocks++; }
static void IOLog(const char *format,...) { (void)format; }
static uint32_t redwood_gpu_submit(const uint64_t *addresses,const uint32_t *words,uint32_t count)
{ backend_submits++;return a750_live_submit(addresses,words,count); }
static void redwood_gpu_report(const char *why) { reports++;a750_live_report(why); }
static bool redwood_gpu_recover(void) { recoveries++;return false; }
enum { kIOReturnBadArgument=-1, kIOReturnSuccess=0, MSM_SUBMIT_CMD_BUF=1 };
static int dispatch(struct msm_gem_submit_cmd *cmds,uint32_t nr_cmds)
{
    struct msm_gem_submit storage={.nr_bos=1,.nr_cmds=nr_cmds};
    struct msm_gem_submit *request=&storage;
    struct msm_gem_submit_bo bos[]={{.handle=1}};
    uint64_t addresses[128];uint32_t words[128],count=0;
    int ret=kIOReturnBadArgument;
    /* From the real command loop through its backend failure handling. */
#include "gpu-submit-validation.inc"
    ret=request->fence?kIOReturnSuccess:-2;
out:
    return ret;
}
static void reset(struct a750_hfi *session)
{
    memset(regs,0,sizeof(regs));memset(ring,0,sizeof(ring));memset(mem,0,sizeof(mem));
    memset(&a750_live,0,sizeof(a750_live));
    a750_live.session=session;a750_live.gx=1;a750_live.ring=ring;a750_live.memptrs=mem;
    a750_live.fence=5;
    backend_submits=reports=recoveries=unlocks=0;gGPUReported=false;
}
int main(void)
{
    struct a750_gx gx={0};struct a750_hfi session={.gx=&gx,.smmu=2,.gmu=3};
    const struct msm_gem_submit_cmd invalid[]={
        {.type=1,.size=0},             /* zero words */
        {.type=2,.size=4},             /* every command filtered out */
        {.type=1,.size=1u<<22},        /* 20-bit word count overflows */
        {.type=1,.size=3},             /* fractional DWORD */
        {.type=1,.size=4,.submit_offset=1},
        {.type=1,.size=4,.submit_idx=1},
        {.type=1,.size=4,.nr_relocs=1},
    };
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        reset(&session);
        struct msm_gem_submit_cmd command=invalid[i];
        assert(dispatch(&command,1)==kIOReturnBadArgument);
        assert(unlocks==1 && !backend_submits && !reports && !recoveries);
        assert(!a750_live.failed && !gGPUReported && a750_live.fence==5 && !regs[0x807]);
        command=(struct msm_gem_submit_cmd){.type=1,.size=4};
        assert(dispatch(&command,1)==kIOReturnSuccess);
        assert(backend_submits==1 && !reports && !a750_live.failed && a750_live.fence==6);
    }
    reset(&session);
    struct msm_gem_submit_cmd largest={.type=1,.size=(1u<<22)-4};
    assert(dispatch(&largest,1)==kIOReturnSuccess && !a750_live.failed);
    puts("GPU validation tests passed: invalid requests never reach failure recovery; valid submission succeeds afterward; IB maximum retained");
    return 0;
}
