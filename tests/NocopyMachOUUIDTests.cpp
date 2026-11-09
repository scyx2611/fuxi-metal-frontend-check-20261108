#include "NocopyMachOUUID.h"
#include <cassert>
#include <cstdio>

template<class T>static void put(uint8_t *p,size_t at,T v){std::memcpy(p+at,&v,sizeof v);}
int main(){
 uint8_t data[80]={};uint8_t output[16];std::memset(output,0xaa,sizeof output);
 put(data,0,uint32_t(0xfeedfacf));put(data,16,uint32_t(2));put(data,20,uint32_t(32));
 put(data,32,uint32_t(0x19));put(data,36,uint32_t(8));
 put(data,40,uint32_t(0x1b));put(data,44,uint32_t(24));
 for(unsigned i=0;i<16;i++)data[48+i]=static_cast<uint8_t>(i+1);
 assert(copyMachOUUID(data,64,output));assert(std::memcmp(output,data+48,16)==0);
 std::memset(output,0xaa,sizeof output);
 assert(!copyMachOUUID(data,63,output));assert(!copyMachOUUID(nullptr,64,output));
 put(data,44,uint32_t(23));assert(!copyMachOUUID(data,64,output));put(data,44,uint32_t(24));
 put(data,36,uint32_t(0));assert(!copyMachOUUID(data,64,output));put(data,36,uint32_t(8));
 put(data,20,uint32_t(31));assert(!copyMachOUUID(data,64,output));put(data,20,uint32_t(32));
 put(data,16,uint32_t(4097));assert(!copyMachOUUID(data,64,output));put(data,16,uint32_t(2));
 put(data,32,uint32_t(0x1b));put(data,36,uint32_t(24));
 assert(!copyMachOUUID(data,64,output));
 uint8_t duplicate[80]={};
 put(duplicate,0,uint32_t(0xfeedfacf));put(duplicate,16,uint32_t(2));put(duplicate,20,uint32_t(48));
 put(duplicate,32,uint32_t(0x1b));put(duplicate,36,uint32_t(24));
 put(duplicate,56,uint32_t(0x1b));put(duplicate,60,uint32_t(24));
 assert(!copyMachOUUID(duplicate,sizeof duplicate,output));
 for(uint8_t b:output)assert(b==0xaa);
 std::puts("PASS: bounded Mach-O UUID parser rejects malformed tables");
}
