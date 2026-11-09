#include "NocopySafeRead.h"
#include <cassert>
#include <cstdio>

int main() {
 vm_size_t page=static_cast<vm_size_t>(vm_page_size);
 vm_address_t region=0;
 assert(page>=0x1000);
 assert(vm_allocate(mach_task_self(),&region,page*2,VM_FLAGS_ANYWHERE)==KERN_SUCCESS);
 assert(vm_protect(mach_task_self(),region+page,page,false,VM_PROT_NONE)==KERN_SUCCESS);
 auto *source=reinterpret_cast<uint8_t *>(region);
 for(size_t i=0;i<0xb0;i++)source[i]=static_cast<uint8_t>(i^0x5a);
 uint8_t copied[0xb0];std::memset(copied,0xcc,sizeof copied);
 assert(copyNocopyObjectHeader(static_cast<uintptr_t>(region),copied));
 assert(std::memcmp(source,copied,sizeof copied)==0);
 std::memset(copied,0xcc,sizeof copied);
 assert(!copyNocopyObjectHeader(0,copied));
 assert(!copyNocopyObjectHeader(static_cast<uintptr_t>(region)+1,copied));
 assert(!copyNocopyObjectHeader(static_cast<uintptr_t>(region+page-0x58),copied));
 for(uint8_t b:copied)assert(b==0xcc);
 assert(vm_deallocate(mach_task_self(),region,page*2)==KERN_SUCCESS);
 std::puts("PASS: bounded self-memory copy refuses invalid and protected ranges");
}
