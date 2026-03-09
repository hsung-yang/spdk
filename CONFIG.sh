#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#

# configure options: __CONFIGURE_OPTIONS__

# Installation prefix
CONFIG[PREFIX]="/usr/local"

# Target architecture
CONFIG[ARCH]=native

# Destination directory for the libraries
CONFIG[LIBDIR]=

# Prefix for cross compilation
CONFIG[CROSS_PREFIX]=

# Build with debug logging. Turn off for performance testing and normal usage
CONFIG[DEBUG]=n

# Treat warnings as errors (fail the build on any warning).
CONFIG[WERROR]=n

# Build with link-time optimization.
CONFIG[LTO]=n

# Generate profile guided optimization data.
CONFIG[PGO_CAPTURE]=n

# Use profile guided optimization data.
CONFIG[PGO_USE]=n

# Place profile data in this directory
CONFIG[PGO_DIR]=

# Build with code coverage instrumentation.
CONFIG[COVERAGE]=n

# Build with Address Sanitizer enabled
CONFIG[ASAN]=n

# Build with Undefined Behavior Sanitizer enabled
CONFIG[UBSAN]=n

# Build with LLVM fuzzing enabled
CONFIG[FUZZER]=n
CONFIG[FUZZER_LIB]=

# Build with Thread Sanitizer enabled
CONFIG[TSAN]=n

# Build functional tests
CONFIG[TESTS]=y

# Build unit tests
CONFIG[UNIT_TESTS]=y

# Build examples
CONFIG[EXAMPLES]=y

# Build apps
CONFIG[APPS]=y

# Build with Control-flow Enforcement Technology (CET)
CONFIG[CET]=n

# Directory that contains the desired SPDK environment library.
# By default, this is implemented using DPDK.
CONFIG[ENV]=

# This directory should contain 'include' and 'lib' directories for your DPDK
# installation.
CONFIG[DPDK_DIR]=
# Automatically set via pkg-config when bare --with-dpdk is set
CONFIG[DPDK_LIB_DIR]=
CONFIG[DPDK_INC_DIR]=
CONFIG[DPDK_PKG_CONFIG]=n

# This directory should contain 'include' and 'lib' directories for WPDK.
CONFIG[WPDK_DIR]=

# Build SPDK FIO plugin. Requires CONFIG_FIO_SOURCE_DIR set to a valid
# fio source code directory.
CONFIG[FIO_PLUGIN]=n

# This directory should contain the source code directory for fio
# which is required for building the SPDK FIO plugin.
CONFIG[FIO_SOURCE_DIR]=/usr/src/fio

# Enable RDMA support for the NVMf target.
# Requires ibverbs development libraries.
CONFIG[RDMA]=n
CONFIG[RDMA_SEND_WITH_INVAL]=n
CONFIG[RDMA_SET_ACK_TIMEOUT]=n
CONFIG[RDMA_SET_TOS]=n
CONFIG[RDMA_PROV]=verbs

# Enable NVMe Character Devices.
CONFIG[NVME_CUSE]=y

# Enable FC support for the NVMf target.
# Requires FC low level driver (from FC vendor)
CONFIG[FC]=n
CONFIG[FC_PATH]=

# Build Ceph RBD support in bdev modules
# Requires librbd development libraries
CONFIG[RBD]=n

# Build DAOS support in bdev modules
# Requires daos development libraries
CONFIG[DAOS]=n
CONFIG[DAOS_DIR]=

# Build UBLK support
CONFIG[UBLK]=n

# Build vhost library.
CONFIG[VHOST]=y

# Build vhost initiator (Virtio) driver.
CONFIG[VIRTIO]=y

# Build custom vfio-user transport for NVMf target and NVMe initiator.
CONFIG[VFIO_USER]=n
CONFIG[VFIO_USER_DIR]=

# Build with xNVMe
CONFIG[XNVME]=n

# Enable the dependencies for building the DPDK accel compress module
CONFIG[DPDK_COMPRESSDEV]=n

# Enable mlx5_pci dpdk crypto PMD, enabled automatically if CONFIG[CRYPTO]=y and libmlx5 exists
CONFIG[CRYPTO_MLX5]=n

# Enable UADK dpdk crypto PMD
CONFIG[DPDK_UADK]=n

# Requires libiscsi development libraries.
CONFIG[ISCSI_INITIATOR]=n

# Enable the dependencies for building the crypto vbdev
CONFIG[CRYPTO]=n

# Build spdk shared libraries in addition to the static ones.
CONFIG[SHARED]=n

# Build with VTune support.
CONFIG[VTUNE]=n
CONFIG[VTUNE_DIR]=

# Build Intel IPSEC_MB library
CONFIG[IPSEC_MB]=n

# Enable OCF module
CONFIG[OCF]=n
CONFIG[OCF_PATH]=
CONFIG[CUSTOMOCF]=n

# Build ISA-L library
CONFIG[ISAL]=y

# Build ISA-L-crypto library
CONFIG[ISAL_CRYPTO]=y

# Build with IO_URING support
CONFIG[URING]=n

# Build IO_URING bdev with ZNS support
CONFIG[URING_ZNS]=n

# Path to custom built IO_URING library
CONFIG[URING_PATH]=

# Path to custom built OPENSSL library
CONFIG[OPENSSL_PATH]=

# Build with RAID5f support
CONFIG[RAID5F]=n

# Build with IDXD support
# In this mode, SPDK fully controls the DSA device.
CONFIG[IDXD]=n

# Build with USDT support
CONFIG[USDT]=n

# Build with IDXD kernel support.
# In this mode, SPDK shares the DSA device with the kernel.
CONFIG[IDXD_KERNEL]=n

# arc4random is available in stdlib.h
CONFIG[HAVE_ARC4RANDOM]=n

# libuuid is available
CONFIG[HAVE_LIBUUID]=n

# uuid_generate_sha1 is available in uuid/uuid.h
CONFIG[HAVE_UUID_GENERATE_SHA1]=n

# Is DPDK using libbsd?
CONFIG[HAVE_LIBBSD]=n

# Is DPDK using libarchive?
CONFIG[HAVE_LIBARCHIVE]=n

# execinfo.h is available
CONFIG[HAVE_EXECINFO_H]=n

# libkeytuils is available
CONFIG[HAVE_KEYUTILS]=n

# OpenSSL has EVP_MAC definitions
CONFIG[HAVE_EVP_MAC]=n

# Path to IPSEC_MB used by DPDK
CONFIG[IPSEC_MB_DIR]=

# Generate Storage Management Agent's protobuf interface
CONFIG[SMA]=n

# Build with Avahi support
CONFIG[AVAHI]=n

# Setup DPDK's RTE_MAX_LCORES
CONFIG[MAX_LCORES]=128

# Maximum number of iobuf NUMA nodes supported
CONFIG[MAX_NUMA_NODES]=1

# Build all Go components
CONFIG[GOLANG]=n

# Build fsdev
CONFIG[FSDEV]=y

# Build with aio fsdev
CONFIG[AIO_FSDEV]=y

# struct stat has st_atime field
CONFIG[HAVE_STRUCT_STAT_ST_ATIM]=n

# struct stat has st_atimespec field
CONFIG[HAVE_STRUCT_STAT_ST_ATIMESPEC]=n

# libc contains the copy_file_range
CONFIG[COPY_FILE_RANGE]=n

# liblz4 is available
CONFIG[HAVE_LZ4]=n

# libaio has the RW flags (version >= 0.3.111)
CONFIG[AIO_HAVE_RW_FLAGS]=n
