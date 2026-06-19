#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2025 Samsung Electronics Co., Ltd. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

TEST_FILE = cpcs_ebpf_ut.c

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/nvmf/cpcs
CFLAGS += -DHAVE_UBPF
CFLAGS += -I/usr/local/include

LDFLAGS += -L/usr/local/lib -L/usr/local/lib64
LIBS += -lubpf

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
