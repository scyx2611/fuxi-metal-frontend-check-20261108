// Exercises the actual frontend's producer, provenance and importer without a
// Vulkan device. Only texture creation/upload are replaced by a byte oracle.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cassert>
#include <cstdint>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
static bool testLayoutUpload(id,const uint64_t[6],NSUInteger,NSUInteger,const void *,NSUInteger,NSUInteger,uint64_t);
#define FUXI_LAYOUT_UPLOAD testLayoutUpload
#ifndef FUXI_NATIVE_FRONTEND
#error Supply the fully prepared NativeMetal frontend
#endif
#include FUXI_NATIVE_FRONTEND
static bool latched,failUpload,failAllocation;
static unsigned failureCalls,uploadCalls;
@interface LayoutTestTexture : NSObject {
 @public std::vector<std::vector<uint8_t>> uploads;
}
@end
@implementation LayoutTestTexture
@end
@interface LayoutTestDevice : NBDeviceMethods
@end
@implementation LayoutTestDevice
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
 (void)descriptor;if(failAllocation)return nil;
 return (id<MTLTexture>)[LayoutTestTexture new];
}
@end
static bool testLayoutUpload(id texture,const uint64_t region[6],NSUInteger mip,NSUInteger slice,
                              const void *bytes,NSUInteger row,NSUInteger image,uint64_t length) {
 (void)mip;(void)slice;assert(region[2]==0 && region[5]==1 && row && image && length==image);
 ++uploadCalls;if(failUpload)return false;
 auto *source=static_cast<const uint8_t *>(bytes);
 ((LayoutTestTexture *)texture)->uploads.emplace_back(source,source+length);return true;
}
static void resetFailure(){latched=false;failureCalls=0;failUpload=false;failAllocation=false;}
static MTLTextureDescriptor *descriptor(NSUInteger width,NSUInteger height,BOOL mips=NO) {
 auto *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR width:width height:height mipmapped:mips];
 d.usage=MTLTextureUsageShaderRead;return d;
}
static void *allocate(NSUInteger bytes) {
 void *memory=nullptr;assert(!posix_memalign(&memory,16384,(bytes+16383)&~NSUInteger(16383)));
 memset(memory,0xcc,(bytes+16383)&~NSUInteger(16383));return memory;
}
static void produce(FuxiLinearTextureLayout *layout,void *memory,MTLTextureDescriptor *d,uint8_t seed=0x35) {
 const auto &plan=[layout blockLayout];
 for(size_t slice=0;slice<plan.descriptor.slices;++slice)for(size_t mip=0;mip<plan.descriptor.levels;++mip) {
  const auto &level=plan.level[mip];std::vector<uint8_t> bytes(level.imageBytes,(uint8_t)(seed+slice*16+mip));
  [layout copyFromLinearBytes:bytes.data() linearOffset:0 linearBytesPerRow:level.rowBytes linearBytesPerImage:level.imageBytes
             toTextureMemory:memory textureSlice:slice textureLevel:mip textureRegion:MTLRegionMake2D(0,0,level.width,level.height)];
 }
 (void)d;
}
int main(){@autoreleasepool{
 unsetenv("FUXI_NATIVE_LINEAR_LAYOUT");
 FuxiNativeHooks hooks={FUXI_NATIVE_ABI,sizeof(FuxiNativeHooks),[](const char *){latched=true;++failureCalls;},[](){return latched;},nullptr};
 fuxi_native_metal_set_hooks(&hooks);auto *dev=[LayoutTestDevice new];
 auto *d=descriptor(1125,2436);
 FuxiLinearTextureLayout *layout=[dev newTextureLayoutWithDescriptor:d isHeapOrBufferBacked:NO];
 assert(layout && !latched && layout.size==2747808 && layout.alignment==16384);
 MTLSize granularity=[layout copyGranularity];assert(granularity.width==4 && granularity.height==4 && granularity.depth==1);
 auto stamp=[layout watermark];assert(!memcmp(&stamp,"Fuxi ASTC pair 1",16));
 void *memory=allocate(layout.size);[layout initializeTextureMemory:memory];assert(!latched);
 // Incomplete producer coverage refuses without reading the claimed source.
 assert(![dev newTextureWithBytesNoCopy:memory length:2752512 descriptor:d deallocator:nil] && latched);
 resetFailure();produce(layout,memory,d);assert(!latched && failureCalls==0);
 // Success messages must not call Fuxi's failure hook.
 __block unsigned released=0;
 @autoreleasepool {
  id<MTLTexture> texture=[dev newTextureWithBytesNoCopy:memory length:2752512 descriptor:d deallocator:^(void *p,NSUInteger n){assert(p==memory && n==2752512);++released;}];
  assert(texture && !latched && !failureCalls && uploadCalls==1);
  auto &bytes=((LayoutTestTexture *)(id)texture)->uploads.at(0);assert(bytes.size()==2747808);
  for(uint8_t byte:bytes)assert(byte==0x35);
  // Imported texture is an owned snapshot, matching upstream's SPI route.
  memset(memory,0x7a,layout.size);assert(bytes.front()==0x35 && bytes.back()==0x35);
  // Retain as a submitted operation would: releasing the caller is insufficient.
  NSMutableArray *inFlight=[NSMutableArray arrayWithObject:texture];texture=nil;assert(released==0);
  [inFlight removeAllObjects];inFlight=nil;
 }
 assert(released==1);
 // Destruction of an owning import removes its producer record.
 std::vector<uint8_t> snapshot;assert(fuxiSnapshotLayout(memory,2752512,[layout blockLayout],d.pixelFormat,snapshot)==FuxiLayoutSnapshot::Unknown);
 free(memory);
 // Unknown unreadable addresses are refused by the bounded read, even with matching length.
 resetFailure();assert(![dev newTextureWithBytesNoCopy:(void *)16384 length:2752512 descriptor:d deallocator:nil] && latched);
 // Fresh complete generation, zero-length inference, short input and upload errors.
 resetFailure();d=descriptor(7,7);layout=[dev newTextureLayoutWithDescriptor:d isHeapOrBufferBacked:NO];memory=allocate(layout.size);
 [layout initializeTextureMemory:memory];produce(layout,memory,d);
 assert(![dev newTextureWithBytesNoCopy:memory length:63 descriptor:d deallocator:nil] && latched);
 resetFailure();id<MTLTexture> texture=[dev newTextureWithBytesNoCopy:memory length:0 descriptor:d deallocator:nil];assert(texture && !latched);
 texture=nil;failUpload=true;
 assert(![dev newTextureWithBytesNoCopy:memory length:64 descriptor:d deallocator:nil] && latched);
 resetFailure();failAllocation=true;
 assert(![dev newTextureWithBytesNoCopy:memory length:64 descriptor:d deallocator:^(void *,NSUInteger){++released;}] && released==1);
 resetFailure();
 // Incomplete partial writes cannot be promoted by writing the same block twice.
 [layout initializeTextureMemory:memory];uint8_t block[16];memset(block,0x1f,sizeof block);
 for(unsigned i=0;i<2;i++)[layout copyFromLinearBytes:block linearOffset:0 linearBytesPerRow:16 linearBytesPerImage:16
                     toTextureMemory:memory textureSlice:0 textureLevel:0 textureRegion:MTLRegionMake2D(0,0,4,4)];
 assert(fuxiSnapshotLayout(memory,64,[layout blockLayout],d.pixelFormat,snapshot)==FuxiLayoutSnapshot::Invalid);
 // Fill each remaining partial edge block, then round-trip into padded rows.
 for(NSUInteger y=0;y<2;y++)for(NSUInteger x=0;x<2;x++)[layout copyFromLinearBytes:block linearOffset:0 linearBytesPerRow:16 linearBytesPerImage:16
  toTextureMemory:memory textureSlice:0 textureLevel:0 textureRegion:MTLRegionMake2D(x*4,y*4,x?3:4,y?3:4)];
 uint8_t linear[96];memset(linear,0xee,sizeof linear);
 [layout copyFromTextureMemory:memory textureSlice:0 textureLevel:0 textureRegion:MTLRegionMake2D(0,0,7,7)
                 toLinearBytes:linear linearOffset:8 linearBytesPerRow:48 linearBytesPerImage:80];
 for(unsigned i=0;i<96;i++)assert(linear[i]==((i>=8 && i<40)||(i>=56 && i<88)?0x1f:0xee));
 [layout finalizeTextureMemory:memory];assert(fuxiSnapshotLayout(memory,64,[layout blockLayout],d.pixelFormat,snapshot)==FuxiLayoutSnapshot::Unknown);free(memory);
 // An invalid copy invalidates provenance and leaves destination sentinels intact.
 memory=allocate(layout.size);[layout initializeTextureMemory:memory];produce(layout,memory,d);
 [layout copyFromLinearBytes:block linearOffset:0 linearBytesPerRow:16 linearBytesPerImage:16 toTextureMemory:memory
                  textureSlice:0 textureLevel:0 textureRegion:MTLRegionMake2D(1,0,4,4)];assert(latched);
 assert(fuxiSnapshotLayout(memory,64,[layout blockLayout],d.pixelFormat,snapshot)==FuxiLayoutSnapshot::Unknown);free(memory);resetFailure();
 // Exact mip/slice provenance and upload order for odd dimensions.
 d=descriptor(9,5,YES);d.textureType=MTLTextureType2DArray;d.arrayLength=2;
 layout=[dev newTextureLayoutWithDescriptor:d isHeapOrBufferBacked:NO];assert(layout && layout.size==288);memory=allocate(layout.size);
 [layout initializeTextureMemory:memory];produce(layout,memory,d,0x40);
 texture=[dev newTextureWithBytesNoCopy:memory length:layout.size descriptor:d deallocator:nil];assert(texture && !latched);
 auto &uploads=((LayoutTestTexture *)(id)texture)->uploads;assert(uploads.size()==8);
 for(size_t i=0;i<8;i++)for(uint8_t byte:uploads[i])assert(byte==0x40+(i/4)*16+i%4);
 [layout finalizeTextureMemory:memory];free(memory);texture=nil;
 // Safe snapshot failure after producer pages become inaccessible preserves output.
 const size_t page=(size_t)getpagesize();const size_t mapping=4*page;
 void *raw=mmap(nullptr,mapping,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(raw!=MAP_FAILED);
 uintptr_t aligned=((uintptr_t)raw+16383)&~uintptr_t(16383);assert(aligned+64<=(uintptr_t)raw+mapping);
 d=descriptor(7,7);layout=[dev newTextureLayoutWithDescriptor:d isHeapOrBufferBacked:NO];memory=(void *)aligned;
 [layout initializeTextureMemory:memory];produce(layout,memory,d);assert(!mprotect(raw,mapping,PROT_NONE));
 snapshot.assign(3,0xab);assert(fuxiSnapshotLayout(memory,64,[layout blockLayout],d.pixelFormat,snapshot)==FuxiLayoutSnapshot::Invalid);
 assert(snapshot.size()==3 && snapshot[0]==0xab);[layout finalizeTextureMemory:memory];assert(!munmap(raw,mapping));
 {
  resetFailure();
  // A. 成功匯入觀察到的 cross-process ASTC 4x4 形狀並驗證獨立 snapshot 與來源覆寫隔離。
  MTLTextureDescriptor *fd=descriptor(1125,2436);uint8_t *fsrc=(uint8_t *)allocate(2752512);
  for(size_t i=0;i<2747808;++i)fsrc[i]=(uint8_t)((i/16)*131u+(i%16)*7u+1u);
  @autoreleasepool {
   const unsigned fUploadsBefore=uploadCalls;
   id<MTLTexture> ftex=[dev newTextureWithBytesNoCopy:fsrc length:2752512 descriptor:fd deallocator:nil];
   assert(ftex && !latched && failureCalls==0 && uploadCalls==fUploadsBefore+1);
   auto &fup=((LayoutTestTexture *)(id)ftex)->uploads;
   assert(fup.size()==1 && fup[0].size()==2747808 && !memcmp(fup[0].data(),fsrc,2747808));
   memset(fsrc,0x5a,2747808);
   for(size_t i=0;i<2747808;++i)assert(fup[0][i]==(uint8_t)((i/16)*131u+(i%16)*7u+1u));
   ftex=nil;
  }
  // B. 成功匯入時 deallocator 於最後一個 strong reference 釋放後才執行一次。
  __block unsigned freleased=0;
  @autoreleasepool {
   id<MTLTexture> fowned=[dev newTextureWithBytesNoCopy:fsrc length:2752512 descriptor:fd deallocator:^(void *p,NSUInteger n){assert(p==fsrc && n==2752512);++freleased;}];
   assert(fowned && !latched && freleased==0);fowned=nil;
  }
  assert(freleased==1);
  // C1. 未對齊 16384 的 foreign base 位址必須被拒絕。
  assert(![dev newTextureWithBytesNoCopy:(uint8_t *)fsrc+16 length:2752512-16 descriptor:fd deallocator:nil] && latched);resetFailure();
  // C2. 長度不足 totalBytes 的 foreign 來源必須被拒絕。
  assert(![dev newTextureWithBytesNoCopy:fsrc length:2747807 descriptor:fd deallocator:nil] && latched);resetFailure();
  // C3. 長度為 0 的 foreign 來源必須被拒絕（不允許零長度推斷）。
  assert(![dev newTextureWithBytesNoCopy:fsrc length:0 descriptor:fd deallocator:nil] && latched);resetFailure();
  // C4. usage 非純粹 MTLTextureUsageShaderRead 時必須被拒絕。
  MTLTextureDescriptor *fusage=[fd copy];fusage.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
  assert(![dev newTextureWithBytesNoCopy:fsrc length:2752512 descriptor:fusage deallocator:nil] && latched);resetFailure();
  // C5. 含有多層 mipmap 的未登錄來源必須被拒絕。
  MTLTextureDescriptor *fmips=descriptor(1125,2436,YES);
  FuxiLinearTextureLayout *fmipsLayout=[dev newTextureLayoutWithDescriptor:fmips isHeapOrBufferBacked:NO];
  assert(fmipsLayout && !latched && fmipsLayout.size>2747808);
  const NSUInteger fmipsSize=fmipsLayout.size;void *fmipsMem=allocate(fmipsSize);
  assert(![dev newTextureWithBytesNoCopy:fmipsMem length:fmipsSize descriptor:fmips deallocator:nil] && latched);
  free(fmipsMem);resetFailure();
  // C6. 含有多個 slice 的 2D array 未登錄來源必須被拒絕。
  MTLTextureDescriptor *farr=descriptor(1125,2436);farr.textureType=MTLTextureType2DArray;farr.arrayLength=2;
  FuxiLinearTextureLayout *farrLayout=[dev newTextureLayoutWithDescriptor:farr isHeapOrBufferBacked:NO];
  assert(farrLayout && !latched && farrLayout.size==2*2747808);
  const NSUInteger farrSize=farrLayout.size;void *farrMem=allocate(farrSize);
  assert(![dev newTextureWithBytesNoCopy:farrMem length:farrSize descriptor:farr deallocator:nil] && latched);
  free(farrMem);resetFailure();
  // C7. 停用 FUXI_NATIVE_FOREIGN_LINEAR 時拒絕，重新啟用後恢復成功。
  setenv("FUXI_NATIVE_FOREIGN_LINEAR","0",1);
  assert(![dev newTextureWithBytesNoCopy:fsrc length:2752512 descriptor:fd deallocator:nil] && latched);
  resetFailure();unsetenv("FUXI_NATIVE_FOREIGN_LINEAR");
  @autoreleasepool {
   id<MTLTexture> frenabled=[dev newTextureWithBytesNoCopy:fsrc length:2752512 descriptor:fd deallocator:nil];
   assert(frenabled && !latched);frenabled=nil;
  }
  resetFailure();
  // C8. 權限設為 PROT_NONE 的不可讀頁面會安全拒絕且不崩潰。
  MTLTextureDescriptor *fsmallDesc=descriptor(7,7);
  const size_t fpage=(size_t)getpagesize();const size_t fmapping=4*fpage;
  void *fraw=mmap(nullptr,fmapping,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(fraw!=MAP_FAILED);
  uintptr_t faligned=((uintptr_t)fraw+16383)&~uintptr_t(16383);assert(faligned+64<=(uintptr_t)fraw+fmapping);
  memset((void *)faligned,0x2b,64);assert(!mprotect(fraw,fmapping,PROT_NONE));
  assert(![dev newTextureWithBytesNoCopy:(void *)faligned length:64 descriptor:fsmallDesc deallocator:nil] && latched);
  assert(!munmap(fraw,fmapping));resetFailure();
  // C9. 遭拒絕的呼叫不得觸發 deallocator。
  __block unsigned frefusedReleased=0;
  assert(![dev newTextureWithBytesNoCopy:fsrc length:2747807 descriptor:fd deallocator:^(void *,NSUInteger){++frefusedReleased;}] && latched && frefusedReleased==0);
  resetFailure();
  // D. upload 失敗時回傳 nil、觸發 failure hook，且不得呼叫 deallocator。
  __block unsigned fuploadFailReleased=0;failUpload=true;
  assert(![dev newTextureWithBytesNoCopy:fsrc length:2752512 descriptor:fd deallocator:^(void *,NSUInteger){++fuploadFailReleased;}] && latched && fuploadFailReleased==0);
  resetFailure();
  // E. 貼圖配置失敗時回傳 nil 且不得呼叫 deallocator。
  __block unsigned fallocFailReleased=0;failAllocation=true;
  assert(![dev newTextureWithBytesNoCopy:fsrc length:2752512 descriptor:fd deallocator:^(void *,NSUInteger){++fallocFailReleased;}] && fallocFailReleased==0);
  resetFailure();
  // F. 非 4 倍數的小尺寸（7x7）foreign ASTC 4x4 來源可正確上傳 64 bytes。
  uint8_t *fsmall=(uint8_t *)allocate(64);for(size_t i=0;i<64;++i)fsmall[i]=(uint8_t)(i*3u+11u);
  @autoreleasepool {
   const unsigned fsmallUploadsBefore=uploadCalls;
   id<MTLTexture> fsmallTex=[dev newTextureWithBytesNoCopy:fsmall length:64 descriptor:fsmallDesc deallocator:nil];
   assert(fsmallTex && !latched && uploadCalls==fsmallUploadsBefore+1);
   auto &fsmallUp=((LayoutTestTexture *)(id)fsmallTex)->uploads;
   assert(fsmallUp.size()==1 && fsmallUp[0].size()==64 && !memcmp(fsmallUp[0].data(),fsmall,64));
   fsmallTex=nil;
  }
  // G. 釋放所有測試配置並確認未留下任何追蹤登錄紀錄。
  free(fsmall);free(fsrc);
  assert(gFuxiLayoutAllocations.empty() && gFuxiLayoutTrackedBytes==0);
  resetFailure();unsetenv("FUXI_NATIVE_FOREIGN_LINEAR");
 }
 setenv("FUXI_NATIVE_LINEAR_LAYOUT","0",1);resetFailure();assert(![dev newTextureLayoutWithDescriptor:d isHeapOrBufferBacked:NO] && latched);
 unsetenv("FUXI_NATIVE_LINEAR_LAYOUT");resetFailure();d.pixelFormat=MTLPixelFormatBGRA8Unorm;
 assert(![dev newTextureLayoutWithDescriptor:d isHeapOrBufferBacked:NO] && latched);
 assert(gFuxiLayoutAllocations.empty() && gFuxiLayoutTrackedBytes==0);
 puts("PASS: actual layout producer, complete provenance, exact snapshots, rollback, error propagation and owner lifecycle, foreign row-major snapshots");
}}
