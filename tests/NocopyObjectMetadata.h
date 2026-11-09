#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

struct NocopyFrame {
 const char *imagePath;
 uint64_t returnOffset;
 uint8_t imageUUID[16];
 uintptr_t objectRegister;
};

// This fixed identity is for the locally inspected QuartzCore image only.
// Duplicate matches fail closed, as do unknown images and excessive frames.
// Each non-null imagePath must be a readable NUL-terminated loader-owned string.
inline bool selectNocopyObjectFrame(const NocopyFrame *frames, size_t count, uintptr_t *out) {
 static constexpr char path[]="/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
 static constexpr uint8_t uuid[16]={0x9e,0x40,0xb2,0xeb,0x26,0x0c,0x3a,0xf1,0xae,0x45,0xa1,0xa5,0x51,0xa8,0xc1,0xb9};
 if(!frames || !out || count>32) return false;
 size_t matches=0;uintptr_t value=0;
 for(size_t i=0;i<count;i++) {
  if(frames[i].imagePath && std::strcmp(frames[i].imagePath,path)==0 && frames[i].returnOffset==0x200000 &&
     std::memcmp(frames[i].imageUUID,uuid,sizeof uuid)==0) {++matches;value=frames[i].objectRegister;}
 }
 if(matches!=1 || !value || value%alignof(uintptr_t)) return false;
 *out=value;return true;
}

// Decode a *copied* object header only after caller identity is checked.
// This never reads texture bytes, calls object methods, or takes ownership.
struct NocopyObjectMetadata {
 uint8_t flags;
 uint64_t claimedBytes;
 uintptr_t cleanupFunction;
};

enum class NocopyObjectResult { Match, InvalidArgument, ShortHeader, PointerMismatch, DimensionMismatch, LinearFlag };

inline NocopyObjectResult inspectNocopyObjectHeader(const uint8_t *header, size_t size, uintptr_t expectedBytes,
                                                    uint32_t width, uint32_t height, NocopyObjectMetadata *out) {
 if(!header || !out || !expectedBytes || !width || !height) return NocopyObjectResult::InvalidArgument;
 if(size < 0xb0) return NocopyObjectResult::ShortHeader;
 uint32_t actualWidth=0,actualHeight=0;
 uintptr_t actualBytes=0,cleanup=0;
 uint64_t claimed=0;
 std::memcpy(&actualWidth,header+0x10,sizeof actualWidth);
 std::memcpy(&actualHeight,header+0x14,sizeof actualHeight);
 std::memcpy(&actualBytes,header+0x58,sizeof actualBytes);
 std::memcpy(&claimed,header+0x60,sizeof claimed);
 std::memcpy(&cleanup,header+0x68,sizeof cleanup);
 if(actualBytes!=expectedBytes) return NocopyObjectResult::PointerMismatch;
 if(actualWidth!=width || actualHeight!=height) return NocopyObjectResult::DimensionMismatch;
 if(header[0xa8]&1) return NocopyObjectResult::LinearFlag;
 *out={header[0xa8],claimed,cleanup};
 return NocopyObjectResult::Match;
}
