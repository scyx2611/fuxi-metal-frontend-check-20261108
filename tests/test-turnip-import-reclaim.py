import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <pthread.h>
#include "NativeAdrenoABI.h"
using VkResult = int;
using kern_return_t = int;
using mach_vm_address_t = uint64_t;
constexpr VkResult VK_SUCCESS=0, VK_NOT_READY=1, VK_ERROR_OUT_OF_DEVICE_MEMORY=-2;
constexpr VkResult VK_ERROR_DEVICE_LOST=-4, VK_ERROR_INVALID_EXTERNAL_HANDLE=-1000072003;
constexpr int KERN_SUCCESS=0, kIOReturnBusy=1;
constexpr int kIOReturnNoMemory=int32_t(0xe00002bd), kIOReturnNoResources=int32_t(0xe00002be);
constexpr int kIOReturnBadArgument=int32_t(0xe00002c2), kIOReturnUnsupported=int32_t(0xe00002c7);
constexpr int TU_BO_ALLOC_NO_FLAGS=0;
#define vk_error(device,result) (result)
#define TU_RMV(...) ((void)0)
static VkResult vk_errorf(const void *,VkResult result,const char *,...) { return result; }
static VkResult vk_device_set_lost(int *,const char *,...) { return VK_ERROR_DEVICE_LOST; }
struct tu_device { int vk; void *darwin_retire; };
struct tu_bo {
 uint32_t gem_handle; uint64_t size,iova; const char *name; int refcnt;
 uint64_t unique_id,darwin_handle; uint32_t darwin_token; void *map; bool never_unmap;
};
struct darwin_cached_bo {
 uint64_t handle,iova,size,stamp,freed_s; void *mapping; uint32_t token;
 bool read_only,cached;
};
struct darwin_retire_state {
 pthread_mutex_t lock;
 VkResult failure;
 darwin_cached_bo *cache;
 unsigned cache_count;
 uint64_t cache_bytes;
 struct { uint64_t evictions,flushes; } cache_stats;
 unsigned pending_count;
};
static tu_bo output_storage;
static darwin_cached_bo entries[2];
static struct { NativeAdrenoAbiParams params; int conn; } dw;
static bool iova_live, imported, permanently_full, iova_full, cache_present;
static unsigned allocations, iova_frees, imports, maps, bo_frees, cpu_unmaps, lease_unmaps;
static int import_error, free_error, map_error, collect_error;
static bool recover_on_free;
static VkResult darwin_collect_retired(tu_device *) { return collect_error; }
static VkResult darwin_allocate_iova(tu_device *,uint64_t size,uint64_t requested,int flags,uint64_t *address) {
 assert(size==32768 && !requested && !flags && !iova_live);
 ++allocations;
 if (iova_full) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
 iova_live=true; *address=0x20000000; return VK_SUCCESS;
}
static void darwin_free_iova(tu_device *,uint64_t address,uint64_t size) {
 assert(size==32768);
 if (address==0x30000000) { assert(!cache_present); return; }
 assert(address==0x20000000 && iova_live); ++iova_frees; iova_live=false;
}
static int mach_task_self() { return 1; }
static int IOConnectUnmapMemory64(int,uint32_t token,int,mach_vm_address_t) {
 assert(token==7); ++cpu_unmaps; return KERN_SUCCESS;
}
static int dw_call(unsigned method,const void *input,size_t bytes,void *out,size_t outbytes) {
 if (method==NativeAdrenoMethodBoImport) {
  assert(bytes==sizeof(NativeAdrenoAbiBoImport) && outbytes==sizeof(NativeAdrenoAbiBoAllocReply));
  const auto &request=*static_cast<const NativeAdrenoAbiBoImport *>(input);
  assert(request.address==0x100000 && request.bytes==32768 && iova_live);
  ++imports;
  if (import_error && (cache_present || permanently_full)) return import_error;
  imported=true; static_cast<NativeAdrenoAbiBoAllocReply *>(out)->handle=1; return KERN_SUCCESS;
 }
 if (method==NativeAdrenoMethodBoMap) {
  assert(imported && bytes==sizeof(NativeAdrenoAbiBoMap) && outbytes==sizeof(NativeAdrenoAbiBoMapReply));
  const auto &request=*static_cast<const NativeAdrenoAbiBoMap *>(input);
  assert(request.handle==1 && request.iova==0x20000000 && request.permissions==3 && !request.writeback && request.shareability==2);
  ++maps; return map_error;
 }
 if (method==NativeAdrenoMethodBoCpuUnmap) {
  const auto &request=*static_cast<const NativeAdrenoAbiBoCpuUnmap *>(input);
  assert(request.handle==2 && request.token==7 && !out && !outbytes);
  ++lease_unmaps; return KERN_SUCCESS;
 }
 assert(method==NativeAdrenoMethodBoFree && !out && !outbytes);
 const auto &request=*static_cast<const NativeAdrenoAbiBoFree *>(input);
 assert(request.handle==2 && cache_present && !iova_live);
 ++bo_frees;
 if (free_error) return free_error;
 cache_present=false;
 if (recover_on_free) iova_full=false;
 return KERN_SUCCESS;
}
static tu_bo *tu_device_lookup_bo(tu_device *,uint32_t handle) { assert(handle==1); return &output_storage; }
static const char *tu_debug_bos_add(tu_device *,uint64_t,const char *name) { return name; }
static void tu_dump_bo_init(tu_device *,tu_bo *) {}
static VkResult darwin_map_failure(tu_device *,uint64_t handle,uint64_t address,uint64_t size,kern_return_t) {
 assert(handle==1 && address==0x20000000 && size==32768); return VK_ERROR_DEVICE_LOST;
}
'''

TEST = r'''
static darwin_retire_state state;
static tu_device device={0,&state};
static void reset(bool cached=true) {
 output_storage={}; entries[0]={2,0x30000000,32768,0,0,reinterpret_cast<void *>(0x400000),7,false,false};
 state.failure=VK_SUCCESS; state.cache=entries; state.cache_count=cached?1:0;
 state.cache_bytes=cached?32768:0; state.cache_stats={}; state.pending_count=1;
 iova_live=imported=permanently_full=iova_full=false; cache_present=cached;
 allocations=iova_frees=imports=maps=bo_frees=cpu_unmaps=lease_unmaps=0;
 import_error=free_error=map_error=collect_error=0; recover_on_free=true;
 dw.params.flags=NativeAdrenoParamsHostImport;
}
static VkResult attempt() {
 tu_bo *output=reinterpret_cast<tu_bo *>(1);
 VkResult result=darwin_bo_init_host(&device,&output,reinterpret_cast<void *>(0x100000),32768);
 assert(output==(result==VK_SUCCESS?&output_storage:nullptr));
 assert(state.pending_count==1);
 assert(pthread_mutex_trylock(&state.lock)==0);
 pthread_mutex_unlock(&state.lock);
 return result;
}
int main() {
 pthread_mutex_init(&state.lock,nullptr);
 for (int error:{kIOReturnNoMemory,kIOReturnNoResources}) {
  reset(); import_error=error;
  assert(attempt()==VK_SUCCESS && allocations==2 && imports==2 && iova_frees==1 && bo_frees==1);
  assert(cpu_unmaps==1 && lease_unmaps==1 && !state.cache_count && !state.cache_bytes);
  assert(state.cache_stats.evictions==1 && state.cache_stats.flushes==1 && imported && iova_live);
  assert(output_storage.map==reinterpret_cast<void *>(0x100000) && output_storage.never_unmap && !output_storage.darwin_token);
  reset(); import_error=error; permanently_full=true;
  assert(attempt()==VK_ERROR_OUT_OF_DEVICE_MEMORY && imports==2 && iova_frees==2 && !iova_live && !imported);
  reset(false); import_error=error; permanently_full=true;
  assert(attempt()==VK_ERROR_OUT_OF_DEVICE_MEMORY && imports==1 && !bo_frees && !iova_live);
 }
 reset(); iova_full=true;
 assert(attempt()==VK_SUCCESS && allocations==2 && imports==1 && bo_frees==1);
 reset(); iova_full=true; recover_on_free=false;
 assert(attempt()==VK_ERROR_OUT_OF_DEVICE_MEMORY && allocations==2 && !imports && bo_frees==1);
 for (int error:{kIOReturnBadArgument,kIOReturnUnsupported,123}) {
  reset(); import_error=error;
  assert(attempt()==VK_ERROR_INVALID_EXTERNAL_HANDLE && imports==1 && !bo_frees && state.cache_count==1 && !iova_live);
 }
 reset(); import_error=kIOReturnNoResources; free_error=kIOReturnBusy;
 assert(attempt()==VK_ERROR_OUT_OF_DEVICE_MEMORY && imports==1 && bo_frees==1 && state.cache_count==1 && !iova_live);
 reset(); import_error=kIOReturnNoResources; free_error=123;
 assert(attempt()==VK_ERROR_DEVICE_LOST && imports==1 && bo_frees==1 && state.cache_count==1 && !iova_live);
 reset(); collect_error=VK_ERROR_DEVICE_LOST;
 assert(attempt()==VK_ERROR_DEVICE_LOST && !imports && !allocations && !bo_frees);
 reset();
 assert(attempt()==VK_SUCCESS && allocations==1 && imports==1 && !bo_frees && state.cache_count==1);
 reset(); map_error=123;
 assert(attempt()==VK_ERROR_DEVICE_LOST && imports==1 && maps==1 && !bo_frees);
 reset(); tu_bo *output=&output_storage;
 assert(darwin_bo_init_host(&device,&output,reinterpret_cast<void *>(0x101000),32768)==VK_ERROR_INVALID_EXTERNAL_HANDLE && !output && !allocations);
 pthread_mutex_destroy(&state.lock);
 puts("PASS: production host-import retries once after idle-cache reclaim; pressure, invalid input, busy, device loss, fences and ownership remain bounded");
}
'''


def function(source, name):
    at = source.index('\n' + name + '(')
    start = source.rfind('\n\n', 0, at) + 2
    body = source.index('{', at)
    end, depth = body + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--turnip-inputs', type=Path, required=True)
    parser.add_argument('--patch', type=Path, required=True)
    parser.add_argument('--abi', type=Path, required=True)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--without-fix', action='store_true')
    arguments = parser.parse_args()
    inputs = arguments.turnip_inputs.resolve()
    upstream = inputs / 'turnip-darwin-kmd.patch'
    expected = 'f3bc6d470f8e0bde1b1d0d370175ec105ee45fe50075e43279581e61b1455c2b'
    if hashlib.sha256(upstream.read_bytes()).hexdigest() != expected:
        raise ValueError('Pinned upstream KMD differs from the production recipe')
    section = upstream.read_text().split('+++ b/src/freedreno/vulkan/tu_knl_darwin.cc\n', 1)[1]
    section = section.split('\ndiff --git ', 1)[0]
    source = ''.join(line[1:] + '\n' for line in section.splitlines()
                     if line.startswith('+') and not line.startswith('+++'))
    with tempfile.TemporaryDirectory(prefix='fuxi-import-reclaim-') as temporary:
        directory = Path(temporary)
        target = directory / 'src/freedreno/vulkan/tu_knl_darwin.cc'
        target.parent.mkdir(parents=True)
        target.write_text(source)
        stack = ['mesa-fuxi-smoke.patch', 'mesa-fuxi-hostimport.patch',
                 'mesa-fuxi-ir3-round-default.patch', 'mesa-fuxi-fence-wait-100ms.patch',
                 'mesa-fuxi-cached-coherent.patch', 'mesa-fuxi-bo-cache.patch']
        command = ['git', 'apply', '--include=src/freedreno/vulkan/tu_knl_darwin.cc']
        for name in stack:
            subprocess.run(command + [str(inputs / name)], cwd=directory, check=True)
        if not arguments.without_fix:
            subprocess.run(command + ['--check', str(arguments.patch.resolve())], cwd=directory, check=True)
            subprocess.run(command + [str(arguments.patch.resolve())], cwd=directory, check=True)
        source = target.read_text()
        if arguments.source and arguments.source.read_text() != source:
            raise ValueError('Built KMD differs from the tested production patch stack')
        names = ['darwin_release_native', 'darwin_cache_evict', 'darwin_cache_flush']
        if not arguments.without_fix:
            names.append('darwin_reclaim_import_cache')
        names.append('darwin_bo_init_host')
        model = PREFIX + ''.join(function(source, name) for name in names) + TEST
        model_path = directory / 'import-reclaim.cpp'
        model_path.write_text(model)
        executable = directory / 'import-reclaim'
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable', '-Wno-missing-field-initializers',
                        '-fsanitize=address,undefined', '-pthread', '-I' + str(arguments.abi.resolve()),
                        str(model_path), '-o', str(executable)], check=True)
        environment = dict(os.environ)
        if sys.platform.startswith('linux'):
            environment['ASAN_OPTIONS'] = 'detect_leaks=1'
        subprocess.run([str(executable)], env=environment, check=True)


if __name__ == '__main__':
    main()
