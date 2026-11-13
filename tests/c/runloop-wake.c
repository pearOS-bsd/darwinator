#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
static CFRunLoopRef rl;
static dispatch_semaphore_t rdy;
static void *th(void *a){ rl=CFRunLoopGetCurrent(); dispatch_semaphore_signal(rdy);
  CFRunLoopSourceContext c={0}; CFRunLoopSourceRef s=CFRunLoopSourceCreate(0,0,&c); CFRunLoopAddSource(rl,s,kCFRunLoopDefaultMode);
  CFRunLoopRun(); return 0; }
int main(void){
 rdy=dispatch_semaphore_create(0); pthread_t t; pthread_create(&t,0,th,0); dispatch_semaphore_wait(rdy,DISPATCH_TIME_FOREVER); usleep(200000);
 dispatch_semaphore_t s=dispatch_semaphore_create(0);
 CFRunLoopPerformBlock(rl,kCFRunLoopCommonModes,^{ printf("block ran\n"); dispatch_semaphore_signal(s); });
 CFRunLoopWakeUp(rl);
 long r=dispatch_semaphore_wait(s,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC));
 printf("wait result %ld\n",r); return 0;}
