#!/usr/bin/env python3
"""Check actual IOSurface result encoding against the iOS 16.7 plane getters."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
source = (root / 'xnu/fuxi-scanout/SweetSurface.cpp').read_text()
patch = (root / 'xnu/patches/0046-sweet-iosurface-iomfb.patch').read_text()
start = source.index('static void\nsweet_surface_fill_result(')
body = source.index('{', start)
end, depth = body + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[start:end]
prefix = r'''
#include <cstdint>
#include <cstring>
#include <strings.h>
#include <cassert>
#include <cstdio>
using std::uint8_t; using std::uint16_t; using std::uint32_t; using std::uint64_t;
#define SWEET_SURFACE_RESULT_SIZE 0xa68
#define SWEET_SURFACE_TIMESTAMPS 0x80
struct sweet_surface_plane {uint32_t width,height,bytesPerRow,offset,size; uint16_t bytesPerElement; uint8_t elementWidth,elementHeight;};
struct SweetSurface {uint32_t id,allocSize,width,height,bytesPerRow,offset,pixelFormat,planeCount; uint16_t bytesPerElement; uint8_t elementWidth,elementHeight; uint32_t cacheMode,parentID; sweet_surface_plane planes[2];};
struct Map {uint64_t value; uint64_t getAddress(){return value;}};
struct SweetSurfaceRef {Map *data,*state;};
static void put32(uint8_t *p,uint32_t o,uint32_t v){memcpy(p+o,&v,4);}
static void put64(uint8_t *p,uint32_t o,uint64_t v){memcpy(p+o,&v,8);}
'''
test = r'''
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
int main(){
 SweetSurface s{}; s.width=1125;s.height=2436;s.allocSize=13893632;s.bytesPerRow=4544;s.planeCount=2;
 s.planes[0]={1125,2436,4544,0,11069184,4,1,1};
 s.planes[1]={1125,2436,1152,11075584,2806272,1,1,1};
 Map data{0x12340000}, state{0x22340000};SweetSurfaceRef ref{&data,&state};
 uint8_t out[SWEET_SURFACE_RESULT_SIZE];memset(out,0xff,sizeof out);sweet_surface_fill_result(&s,&ref,out);
 assert(get32(out+0x38)==2);
 for(unsigned i=0;i<2;i++){
  const auto &p=s.planes[i];const uint8_t *r=out+0x68+i*0x80;
  assert(get32(r)==p.width && get32(r+4)==p.height && get32(r+0x0c)==p.offset);
  assert(get32(r+0x10)==p.bytesPerRow && get32(r+0x14)==p.size);
  assert(r[0x18]==p.bytesPerElement && r[0x1a]==p.elementWidth && r[0x1b]==p.elementHeight);
  assert(r[0x54]==0); // iOS getter's per-plane invalidity flag
 }
 assert(get32(out+0x68+0x10)==4544 && get32(out+0xe8+0x10)==1152);
 s.planeCount=0;memset(out,0xff,sizeof out);sweet_surface_fill_result(&s,&ref,out);
 assert(get32(out+0x38)==0 && get32(out+0x28)==4544 && get32(out+0x78)==0);
 puts("PASS: production result encoder supplies both iOS plane strides and preserves other fields");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    src.write_text(prefix + function + '\n' + test)
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
assert source.count('put32(p, 0x10, pl->bytesPerRow);') == 1
assert patch.count('+\t\tput32(p, 0x10, pl->bytesPerRow);') == 1
