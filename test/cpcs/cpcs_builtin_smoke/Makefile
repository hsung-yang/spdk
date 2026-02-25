#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Samsung Electronics Co., Ltd.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = cpcs_builtin_smoke
C_SRCS = cpcs_builtin_smoke.c ../cpcs_common.c

CFLAGS += -I$(SPDK_ROOT_DIR)/test/cpcs
CFLAGS += -I$(SPDK_ROOT_DIR)/lib/nvmf/cpcs

SPDK_LIB_LIST = bdev bdev_slm nvmf

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
