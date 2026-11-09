#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Decode a copied little-endian Mach-O header and load-command table.
// Caller provides a stable local copy, never an untrusted remote pointer.
inline bool copyMachOUUID(const uint8_t *bytes,size_t length,uint8_t (&uuid)[16]) {
 if(!bytes || length<32) return false;
 uint32_t magic=0,ncmds=0,commandBytes=0;
 std::memcpy(&magic,bytes,4);std::memcpy(&ncmds,bytes+16,4);std::memcpy(&commandBytes,bytes+20,4);
 if(magic!=0xfeedfacf || !ncmds || ncmds>4096 || commandBytes>65536 || commandBytes>length-32) return false;
 size_t offset=32,end=32+static_cast<size_t>(commandBytes);unsigned matches=0;uint8_t found[16]{};
 for(uint32_t i=0;i<ncmds;i++) {
  if(offset>end || end-offset<8)return false;
  uint32_t cmd=0,size=0;std::memcpy(&cmd,bytes+offset,4);std::memcpy(&size,bytes+offset+4,4);
  if(size<8 || size%8 || size>end-offset)return false;
  if(cmd==0x1b) {
   if(size!=24)return false;
   ++matches;std::memcpy(found,bytes+offset+8,16);
  }
  offset+=size;
 }
 if(offset!=end || matches!=1)return false;
 std::memcpy(uuid,found,sizeof found);return true;
}
