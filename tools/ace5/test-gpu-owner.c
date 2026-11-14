/* Real owner lifecycle with controlled publication/stop interleavings.
 * No hardware is emulated or accessed; only the kernel scheduling API is stubbed.
 * Run through test-gpu-lifecycle.py with assertions enabled. */
#ifdef NDEBUG
#error "Compile this test with -UNDEBUG"
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION host_mutex;
typedef CONDITION_VARIABLE host_condition;
typedef HANDLE host_thread;
#define HOST_THREAD(name) static DWORD WINAPI name(LPVOID argument)
static void mutex_init(host_mutex *m) { InitializeCriticalSection(m); }
static void mutex_lock(host_mutex *m) { EnterCriticalSection(m); }
static void mutex_unlock(host_mutex *m) { LeaveCriticalSection(m); }
static void condition_init(host_condition *c) { InitializeConditionVariable(c); }
static void condition_wake(host_condition *c) { WakeAllConditionVariable(c); }
static uint64_t now_ms(void) { return GetTickCount64(); }
static bool condition_wait(host_condition *c,host_mutex *m,uint64_t until)
{
    uint64_t now=now_ms();
    if (now>=until) return false;
    return SleepConditionVariableCS(c,m,(DWORD)(until-now))!=0;
}
static host_thread spawn(LPTHREAD_START_ROUTINE fn)
{
    host_thread thread=CreateThread(NULL,0,fn,NULL,0,NULL); assert(thread); return thread;
}
static void join(host_thread thread)
{
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CloseHandle(thread);
}
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t host_mutex;
typedef pthread_cond_t host_condition;
typedef pthread_t host_thread;
#define HOST_THREAD(name) static void *name(void *argument)
static void mutex_init(host_mutex *m) { assert(!pthread_mutex_init(m,NULL)); }
static void mutex_lock(host_mutex *m) { assert(!pthread_mutex_lock(m)); }
static void mutex_unlock(host_mutex *m) { assert(!pthread_mutex_unlock(m)); }
static void condition_init(host_condition *c) { assert(!pthread_cond_init(c,NULL)); }
static void condition_wake(host_condition *c) { assert(!pthread_cond_broadcast(c)); }
static uint64_t now_ms(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_REALTIME,&t));
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static bool condition_wait(host_condition *c,host_mutex *m,uint64_t until)
{
    struct timespec t={(time_t)(until/1000),(long)(until%1000)*1000000};
    return pthread_cond_timedwait(c,m,&t)==0;
}
static host_thread spawn(void *(*fn)(void *))
{
    host_thread thread; assert(!pthread_create(&thread,NULL,fn,NULL)); return thread;
}
static void join(host_thread thread) { assert(!pthread_join(thread,NULL)); }
#endif

typedef uintptr_t vm_offset_t;
typedef void *event_t;
typedef int wait_result_t, thread_t;
typedef void *DTEntry;
enum { kSuccess=1, KERN_SUCCESS=0, LCK_SLEEP_DEFAULT=0, THREAD_UNINT=0, THREAD_TIMED_OUT=1 };
#define NSEC_PER_SEC 1000000000u
#define LCK_GRP_DECLARE(name,label) int name
#define LCK_MTX_DECLARE(name,group) host_mutex name
static host_condition owner_changed;
static _Thread_local unsigned owner_lock_depth;
static void lck_mtx_lock(host_mutex *m) { mutex_lock(m);owner_lock_depth++; }
static void lck_mtx_unlock(host_mutex *m) { owner_lock_depth--;mutex_unlock(m); }
static void thread_wakeup(event_t event) { (void)event;condition_wake(&owner_changed); }
static void clock_interval_to_deadline(unsigned n,unsigned scale,uint64_t *end)
{
    assert(scale==NSEC_PER_SEC); *end=now_ms()+n*100; /* shorten failure deadlines */
}
static int lck_mtx_sleep_deadline(host_mutex *m,int flags,event_t event,int interruptible,uint64_t deadline)
{
    (void)flags;(void)event;(void)interruptible;
    return condition_wait(&owner_changed,m,deadline)?0:THREAD_TIMED_OUT;
}
static void lck_mtx_sleep(host_mutex *m,int flags,event_t event,int interruptible)
{
    (void)flags;(void)event;(void)interruptible;
    assert(condition_wait(&owner_changed,m,now_ms()+3000));
}
static void panic(const char *message) { fprintf(stderr,"%s\n",message);abort(); }
static int a750_log(const char *format,...) { (void)format;return 0; }
static int SecureDTLookupEntry(DTEntry root,const char *path,DTEntry *entry)
{ (void)root;(void)path;(void)entry;return -1; }
static int SecureDTGetProperty(DTEntry entry,const char *name,const void **prop,unsigned *size)
{ (void)entry;(void)name;(void)prop;(void)size;return -1; }
static vm_offset_t pe_arm_get_soc_base_phys(void) { return 0; }
static bool PE_parse_boot_argn(const char *name,void *value,unsigned size)
{ (void)name;(void)value;(void)size;return false; }
static int kernel_thread_start(void (*fn)(void *,wait_result_t),void *arg,thread_t *thread)
{ (void)fn;(void)arg;(void)thread;assert(false);return -1; }
static void thread_deallocate(thread_t thread) { (void)thread;assert(false); }
static thread_t current_thread(void) { return 1; }
static void thread_terminate(thread_t thread) { assert(thread==1); }
struct a750_memory { unsigned unused; };
struct a750_gx { struct a750_memory memory; };
struct a750_hfi { struct a750_gx *gx; };
static unsigned a750_live_phase,a750_gpu_step_delay,a750_gpu_keep;
static bool a750_runtime_active;
static struct {
    struct a750_hfi *session;
    vm_offset_t gx;
    uint32_t *ring;
    volatile uint32_t *memptrs;
    unsigned wptr;
    uint32_t fence;
} a750_live;
static uint32_t ring[8192],memptrs[1024];
static vm_offset_t a750_find_memory(struct a750_memory *m,uint32_t va,uint32_t size)
{
    (void)m;(void)size;return va==0x61040000?(vm_offset_t)ring:(vm_offset_t)memptrs;
}
static unsigned drains;
static bool a750_direct_wait(struct a750_hfi *s,vm_offset_t gx,volatile uint32_t *mem,unsigned wptr,uint32_t fence)
{
    assert(s==a750_live.session && gx==1 && mem==memptrs && wptr==112 && fence==5);
    drains++;return true;
}
static bool a750_gpu_idle(struct a750_hfi *s,vm_offset_t gx) { (void)s;assert(gx==1);return true; }
static const int a750_backend;
static host_mutex publication_lock;
static host_condition publication_changed;
static bool publishing,publication_released;
static void redwood_gpu_use_backend(const void *backend)
{
    assert(backend==&a750_backend && owner_lock_depth==0);
    mutex_lock(&publication_lock);
    publishing=true;condition_wake(&publication_changed);
    while (!publication_released)
        assert(condition_wait(&publication_changed,&publication_lock,now_ms()+3000));
    mutex_unlock(&publication_lock);
}
#include "../../xnu/files/bsd/dev/giulia_gpu_owner.h"
static bool a750_probe(void)
{
    struct a750_gx gx={0};struct a750_hfi session={.gx=&gx};
    return a750_keep_wait(&session,1,112,5);
}
HOST_THREAD(owner_thread) { (void)argument;a750_owner(NULL,0);return 0; }
HOST_THREAD(stop_thread) { (void)argument;a750_live_stop();return 0; }

static void wait_for_phase(unsigned phase)
{
    lck_mtx_lock(&a750_live_lock);
    uint64_t deadline=now_ms()+3000;
    while (a750_live_phase!=phase)
        assert(condition_wait(&owner_changed,&a750_live_lock,deadline));
    lck_mtx_unlock(&a750_live_lock);
}
int main(void)
{
    mutex_init(&a750_live_lock);condition_init(&owner_changed);
    mutex_init(&publication_lock);condition_init(&publication_changed);
    for (unsigned early=0;early<2;early++) {
        a750_live_phase=0;drains=0;memset(&a750_live,0,sizeof(a750_live));
        publishing=false;publication_released=!early;
        host_thread owner=spawn(owner_thread);
        mutex_lock(&publication_lock);
        while (!publishing)
            assert(condition_wait(&publication_changed,&publication_lock,now_ms()+3000));
        mutex_unlock(&publication_lock);
        if (!early) wait_for_phase(1);
        host_thread stopper=spawn(stop_thread);
        if (early) {
            /* Stop while publication is blocked, before keep_wait can announce ready. */
            wait_for_phase(2);
            mutex_lock(&publication_lock);publication_released=true;
            condition_wake(&publication_changed);mutex_unlock(&publication_lock);
        }
        join(owner);join(stopper);
        assert(a750_live_phase==3 && !a750_live.session && drains==1);
        a750_live_stop(); /* repeated stop remains a completed no-op */
    }
    puts("GPU owner tests passed: ready stop, publication-time stop retained, drain once, repeated stop, publication outside lock");
    return 0;
}
