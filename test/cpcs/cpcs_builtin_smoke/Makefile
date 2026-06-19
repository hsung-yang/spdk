#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Samsung Electronics Co., Ltd.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = cpcs_builtin_smoke
# The smoke uses only the public vSLM API (include/spdk/bdev_vslm.h), so link
# libspdk_bdev_vslm.a rather than also compiling vbdev_vslm.c directly -- the
# latter caused multiple-definition link errors (e.g. bdev_vslm_get_stats,
# SPDK_LOG_vslm) because the object was pulled in both directly and via the lib.
C_SRCS = cpcs_builtin_smoke.c ../cpcs_common.c

CFLAGS += -I$(SPDK_ROOT_DIR)/test/cpcs
CFLAGS += -I$(SPDK_ROOT_DIR)/lib/nvmf/cpcs
CFLAGS += -I$(SPDK_ROOT_DIR)/module

SPDK_LIB_LIST = accel bdev bdev_malloc bdev_slm bdev_vslm nvmf

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
