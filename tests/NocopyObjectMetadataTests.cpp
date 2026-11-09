#include "NocopyObjectMetadata.h"
#include <cassert>
#include <cstdio>
#include <cstring>

template<class T> static void put(uint8_t *p,size_t offset,T value) {std::memcpy(p+offset,&value,sizeof value);}
int main() {
 const char *path="/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
 const uint8_t uuid[16]={0x9e,0x40,0xb2,0xeb,0x26,0x0c,0x3a,0xf1,0xae,0x45,0xa1,0xa5,0x51,0xa8,0xc1,0xb9};
 NocopyFrame frames[2]{};frames[0].imagePath=path;frames[0].returnOffset=0x200000;
 std::memcpy(frames[0].imageUUID,uuid,sizeof uuid);frames[0].objectRegister=0x200000;
 uintptr_t selected=0x42;
 assert(selectNocopyObjectFrame(frames,1,&selected) && selected==0x200000);
 frames[1]=frames[0];selected=0x42;
 assert(!selectNocopyObjectFrame(frames,2,&selected) && selected==0x42);
 frames[0].imageUUID[0]^=1;
 assert(!selectNocopyObjectFrame(frames,1,&selected));frames[0].imageUUID[0]^=1;
 frames[0].returnOffset++;
 assert(!selectNocopyObjectFrame(frames,1,&selected));frames[0].returnOffset--;
 frames[0].imagePath="/unexpected/QuartzCore";
 assert(!selectNocopyObjectFrame(frames,1,&selected));frames[0].imagePath=path;
 frames[0].objectRegister=0x200001;
 assert(!selectNocopyObjectFrame(frames,1,&selected));
 assert(!selectNocopyObjectFrame(frames,kNocopyMaxFrames+1,&selected));
 assert(!selectNocopyObjectFrame(nullptr,1,&selected));
 assert(!selectNocopyObjectFrame(frames,1,nullptr));
 uint8_t object[0xb0]={}; NocopyObjectMetadata out{0xaa,0xbb,0xcc};
 const uintptr_t pointer=0x100000; const uint32_t width=1125,height=2436;
 put(object,0x10,width);put(object,0x14,height);put(object,0x58,pointer);
 put(object,0x60,uint64_t(2747808));put(object,0x68,uintptr_t(0x200000));
 object[0xa8]=2;
 assert(inspectNocopyObjectHeader(object,sizeof object,pointer,width,height,&out)==NocopyObjectResult::Match);
 assert(out.flags==2 && out.claimedBytes==2747808 && out.cleanupFunction==0x200000);
 const auto original=out;
 assert(inspectNocopyObjectHeader(object,sizeof object,0,width,height,&out)==NocopyObjectResult::InvalidArgument);
 assert(inspectNocopyObjectHeader(nullptr,sizeof object,pointer,width,height,&out)==NocopyObjectResult::InvalidArgument);
 assert(inspectNocopyObjectHeader(object,0xaf,pointer,width,height,&out)==NocopyObjectResult::ShortHeader);
 assert(inspectNocopyObjectHeader(object,sizeof object,pointer+1,width,height,&out)==NocopyObjectResult::PointerMismatch);
 assert(inspectNocopyObjectHeader(object,sizeof object,pointer,width+1,height,&out)==NocopyObjectResult::DimensionMismatch);
 object[0xa8]=3;
 assert(inspectNocopyObjectHeader(object,sizeof object,pointer,width,height,&out)==NocopyObjectResult::LinearFlag);
 assert(out.flags==original.flags && out.claimedBytes==original.claimedBytes && out.cleanupFunction==original.cleanupFunction);
 std::puts("PASS: only matching copied object headers produce metadata");
}
