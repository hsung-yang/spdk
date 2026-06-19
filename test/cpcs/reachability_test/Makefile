#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = reachability_test
C_SRCS = reachability_test.c ../cpcs_common.c

CFLAGS += -I$(SPDK_ROOT_DIR)/test/cpcs
CFLAGS += -I$(SPDK_ROOT_DIR)/lib/nvmf/cpcs

SPDK_LIB_LIST = bdev bdev_slm nvmf

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
