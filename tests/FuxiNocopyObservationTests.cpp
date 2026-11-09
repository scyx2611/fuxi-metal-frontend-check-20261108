#include "FuxiNocopyObservation.h"
#include <cassert>
#include <cstdio>
#include <cstring>

template<typename T> static void put(uint8_t *p,size_t offset,T value) {std::memcpy(p+offset,&value,sizeof value);}
int main() {
 vm_address_t region=0;vm_size_t page=vm_page_size;
 assert(page>=4096 && vm_allocate(mach_task_self(),&region,page*2,VM_FLAGS_ANYWHERE)==KERN_SUCCESS);
 assert(vm_protect(mach_task_self(),region+page,page,false,VM_PROT_NONE)==KERN_SUCCESS);
 auto *p=reinterpret_cast<uint8_t *>(region);std::memset(p,0,page);
 put(p,0,uint32_t(0xfeedfacf));put(p,16,uint32_t(1));put(p,20,uint32_t(24));
 put(p,32,uint32_t(0x1b));put(p,36,uint32_t(24));
 for(unsigned i=0;i<16;i++)p[40+i]=static_cast<uint8_t>(i+1);
 uint8_t uuid[16]{};assert(fuxiImageUUID(region,uuid));assert(std::memcmp(uuid,p+40,16)==0);
 put(p,20,uint32_t(65537));assert(!fuxiImageUUID(region,uuid));put(p,20,uint32_t(24));
 assert(!fuxiImageUUID(region+page-16,uuid));
 uint8_t copy[8]{};assert(fuxiSafeRead(region,copy,sizeof copy));
 assert(!fuxiSafeRead(region+page-4,copy,sizeof copy));assert(!fuxiSafeRead(0,copy,sizeof copy));
 assert(!fuxiSafeRead(region,copy,65569));
 FuxiNocopyObservation result{};result.metadata.flags=0xa5;result.metadata.claimedBytes=0x1234;
 assert(!fuxiObserveNocopy(p,1125,2436,result));
 assert(result.metadata.flags==0xa5 && result.metadata.claimedBytes==0x1234);
 assert(vm_deallocate(mach_task_self(),region,page*2)==KERN_SUCCESS);
 std::puts("PASS: observation rejects unknown callers and bounds image reads");
}
