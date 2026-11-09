/* Mac tests against the actual externally patched NativeMetal frontend.
 * No Vulkan device is opened; null handles and post-latch CPU bookkeeping only. */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <objc/message.h>
struct NmbDevice;
static unsigned answerMode;
static const char *fakeSelector(size_t index,uint32_t *kind);
static bool fakeAnswer(const NmbDevice *,const char *,uint64_t,uint64_t *out);
static const char *fakeError();
#define FUXI_NMB_ANSWER_SELECTOR fakeSelector
#define FUXI_NMB_DEVICE_ANSWER fakeAnswer
#define FUXI_NMB_LAST_ERROR fakeError
#ifndef FUXI_NATIVE_FRONTEND
#error Supply the prepared external NativeBridgeMetalFrontend.mm
#endif
#include FUXI_NATIVE_FRONTEND
static const char *fakeSelector(size_t index,uint32_t *kind){
 if(index)return nullptr;
 *kind=NMB_ANSWER_COUNT;return "fuxiTestAnswer";
}
static bool fakeAnswer(const NmbDevice *,const char *,uint64_t,uint64_t *out){
 if(answerMode==0){*out=7;return true;}return false;
}
static const char *fakeError(){
 return answerMode==1?"the device has no answer for fuxiTestAnswer":"runtime failure";
}
static bool latched;
static unsigned completions;
static std::vector<std::string> failures;
static void failed(const char *why){failures.emplace_back(why);latched=true;}
int main(){@autoreleasepool{
 FuxiNativeHooks hooks={FUXI_NATIVE_ABI,sizeof(FuxiNativeHooks),failed,[](){return latched;},[](uint64_t,double,uint64_t,bool){++completions;}};
 fuxi_native_metal_set_hooks(&hooks);
 NBDeviceMethods *methods=[NBDeviceMethods new];id<MTLDevice> dev=(id)methods;
 Class known=objc_allocateClassPair([NBDeviceMethods class],"NativeKnownAnswerTest",0);
 objc_registerClassPair(known);answerMode=0;installAnswers(known);
 id knownDevice=[known new];assert(((NSUInteger (*)(id,SEL))objc_msgSend)(knownDevice,sel_registerName("fuxiTestAnswer"))==7 && failures.empty());
 Class unknown=objc_allocateClassPair([NBDeviceMethods class],"NativeUnknownAnswerTest",0);
 objc_registerClassPair(unknown);answerMode=1;installAnswers(unknown);
 SEL missing=sel_registerName("fuxiTestAnswer");assert(!class_getInstanceMethod(unknown,missing) && failures.empty());
 id unknownDevice=[unknown new];
 assert(((NSUInteger (*)(id,SEL))objc_msgSend)(unknownDevice,missing)==0);
 assert(latched && failures.back()=="missing device fuxiTestAnswer");
 failures.clear();latched=false;answerMode=2;installAnswers(unknown);
 assert(latched && !failures.empty());failures.clear();latched=false;
 NSError *error=nil;
 assert(![(id)dev newPipelineLibraryWithFilePath:@"optional.pipelinelib" error:&error] && error && failures.empty());
 (void)nbError(@"descriptor refused");assert(latched && failures.back()=="descriptor refused");
 refuseSurface(1,nullptr,nil,1,0,0);assert(failures.back().find("IOSurface refused")!=std::string::npos);
 error=nil;id<MTLRenderPipelineState> render=[dev newRenderPipelineStateWithDescriptor:[MTLRenderPipelineDescriptor new] error:&error];
 assert(render && !error && ![(NBRenderPipeline *)render handle]);
 id<MTLComputePipelineState> compute=[dev newComputePipelineStateWithFunction:(id)[NSObject new] error:&error];
 assert(compute && !error && ![(NBComputePipeline *)compute handle]);
 NBCommandBuffer *command=[NBCommandBuffer new];__block unsigned scheduledCount=0,completedCount=0;
 [command addScheduledHandler:^(id<MTLCommandBuffer> cb){assert(cb.status==MTLCommandBufferStatusCompleted);++scheduledCount;}];
 [command addCompletedHandler:^(id<MTLCommandBuffer> cb){
  assert(cb.status==MTLCommandBufferStatusCompleted && !cb.error);++completedCount;
  [cb addCompletedHandler:^(id<MTLCommandBuffer> inner){assert(inner==cb);++completedCount;}];
 }];
 [command commit];[command waitUntilScheduled];[command waitUntilCompleted];
 assert(command.fuxiNoop && scheduledCount==1 && completedCount==2 && !completions);
 [command addCompletedHandler:^(id<MTLCommandBuffer> cb){assert(cb==(id)command);++completedCount;}];
 assert(completedCount==3);
 NBCommandBuffer *racing=[NBCommandBuffer new];NBQueue *queue=[NBQueue new];
 queue.completionQueue=dispatch_queue_create("fuxi.callback.test",DISPATCH_QUEUE_SERIAL);racing.queue=queue;
 dispatch_semaphore_t entered=dispatch_semaphore_create(0),release=dispatch_semaphore_create(0);
 __block unsigned racingCount=0;
 [racing addCompletedHandler:^(id<MTLCommandBuffer> cb){
  assert(cb==(id)racing);++racingCount;dispatch_semaphore_signal(entered);
  assert(!dispatch_semaphore_wait(release,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)));
 }];
 [racing commit];assert(!dispatch_semaphore_wait(entered,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)));
 [racing addCompletedHandler:^(id<MTLCommandBuffer> cb){assert(cb==(id)racing);++racingCount;}];
 dispatch_semaphore_signal(release);[racing waitUntilCompleted];assert(racingCount==2);
 size_t before=failures.size();
 @try {[[NBObject new] doesNotRecognizeSelector:sel_registerName("n1Unsupported")];assert(false);}
 @catch(NSException *e){assert(e && failures.size()>before);}
 puts("PASS: actual native refusal/surface/exception hooks, optional fallback, survey placeholders and no-op completion");
}}
