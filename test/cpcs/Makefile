#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 CPCS Implementation Team.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = cpcs_builtin_smoke

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)

clean:
	@for dir in $(DIRS-y); do \
		$(MAKE) -C $$dir clean; \
	done

$(DIRS-y): %:
	$(MAKE) -C $@

install: ;

uninstall: ;
