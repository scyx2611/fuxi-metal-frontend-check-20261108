#pragma once
#include <mach/mach.h>
#include <cstdint>
#include <cstring>

// Read a bounded metadata header from this process without dereferencing it.
// On error or short read, leave the caller's output untouched.
inline bool copyNocopyObjectHeader(uintptr_t address, uint8_t (&out)[0xb0]) {
 if(!address || address % alignof(uintptr_t)) return false;
 uint8_t copy[0xb0];vm_size_t copied=0;
 kern_return_t result=vm_read_overwrite(mach_task_self(),static_cast<vm_address_t>(address),
    sizeof copy,reinterpret_cast<vm_address_t>(copy),&copied);
 if(result!=KERN_SUCCESS || copied!=sizeof copy) return false;
 std::memcpy(out,copy,sizeof copy);return true;
}
