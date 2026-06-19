#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = tcp.c ctrlr.c subsystem.c ctrlr_discovery.c ctrlr_bdev.c nvmf.c auth.c

UBPF_PKG := $(shell $(PKGCONF) --exists ubpf && echo y)
UBPF_AVAILABLE := $(if $(UBPF_PKG),y,$(shell test -f /usr/local/include/ubpf.h -a \( -f /usr/local/lib/libubpf.a -o -f /usr/local/lib/libubpf.so -o -f /usr/local/lib64/libubpf.a -o -f /usr/local/lib64/libubpf.so \) && echo y))
DIRS-$(UBPF_AVAILABLE) += cpcs_ebpf.c

DIRS-$(CONFIG_RDMA) += rdma.c transport.c

DIRS-$(CONFIG_FC) += fc.c fc_ls.c

DIRS-$(CONFIG_VFIO_USER) += vfio_user.c

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
