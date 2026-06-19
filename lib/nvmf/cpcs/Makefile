#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 CPCS Implementation Team.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = nvmf_cpcs.c reachability.c memory_range_set.c program.c program_activation.c \
	 builtin_programs.c builtin_runtime.c runtime_stub.c ebpf_runtime.c execute.c \
	 cpcs_cmd.c admin_cmd.c cpcs_rpc.c
LIBNAME = nvmf_cpcs

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
