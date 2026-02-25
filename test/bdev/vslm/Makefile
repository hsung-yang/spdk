#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = vslm_smoke

C_SRCS := vslm_smoke.c \
	$(SPDK_ROOT_DIR)/module/bdev/vslm/vbdev_vslm.c \
	$(SPDK_ROOT_DIR)/module/bdev/slm/vbdev_slm.c

CFLAGS += -I$(SPDK_ROOT_DIR)/module

SPDK_LIB_LIST = bdev bdev_malloc

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
