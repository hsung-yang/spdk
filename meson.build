project('spdk', 'c', version: '24.05')
fs = import('fs')
cc = meson.get_compiler('c')

math_dep    = cc.find_library('m',       has_headers: ['math.h'])
ssl_dep     = dependency('openssl',      version: '>=1.1.1')
dlfcn_dep   = cc.find_library('dl',      has_headers: ['dlfcn.h'])
uuid_dep    = cc.find_library('uuid',    has_headers: ['uuid/uuid.h'])
numa_dep    = cc.find_library('numa',    has_headers: ['numaif.h'], required: false)
archive_dep = cc.find_library('archive', has_headers: ['archive.h'], required: false)

spdk_lib_dir = meson.current_source_dir() / 'build' / 'lib'
dpdk_lib_dir = meson.current_source_dir() / 'dpdk/build' / 'lib'
isal_lib_dir = meson.current_source_dir() / 'isa-l' / '.libs'

spdk_libnames = [
    'spdk_nvme', 'spdk_env_dpdk',
    'spdk_sock', 'spdk_sock_posix',
    'spdk_thread',
    'spdk_rpc', 'spdk_trace',
    'spdk_jsonrpc', 'spdk_json', 'spdk_util', 'spdk_log',
    'spdk_dma', 'spdk_keyring', 'spdk_vfio_user',
]
dpdk_libnames = [
    'rte_eal', 'rte_log', 'rte_telemetry',
    'rte_bus_pci', 'rte_pci', 'rte_ring', 'rte_mempool', 'rte_kvargs',
]
isal_libnames = ['isal']

spdk_deps  = [dlfcn_dep, math_dep, numa_dep, uuid_dep, ssl_dep, archive_dep]
spdk_paths = []

foreach libname : spdk_libnames
    lib = cc.find_library(libname, dirs: [spdk_lib_dir], static: true, required: false)
    if lib.found()
        p = spdk_lib_dir / 'lib' + libname + '.a'
        if fs.exists(p)
            spdk_paths += p
        endif
    endif
endforeach

foreach libname : dpdk_libnames
    lib = cc.find_library(libname, dirs: [dpdk_lib_dir], static: true, required: false)
    if lib.found()
        p = dpdk_lib_dir / 'lib' + libname + '.a'
        if fs.exists(p)
            spdk_paths += p
        endif
    endif
endforeach

foreach libname : isal_libnames
    lib = cc.find_library(libname, dirs: [isal_lib_dir], static: true, required: false)
    if lib.found()
        p = isal_lib_dir / 'lib' + libname + '.a'
        if fs.exists(p)
            spdk_paths += p
        endif
    endif
endforeach

spdk_inc = include_directories('build/include', 'dpdk/build/include')
spdk_link_args = ['-Wl,--whole-archive'] + spdk_paths + ['-Wl,--no-whole-archive']

spdk_dep = declare_dependency(
    dependencies: spdk_deps,
    link_args: spdk_link_args,
    include_directories: spdk_inc,
)
