/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * NVMe Computational Programs Command Set (CPCS) specification structures
 * Based on NVM Express Computational Programs Command Set Specification, Revision 1.1 (March 2025)
 */

#ifndef SPDK_NVME_CPCS_SPEC_H
#define SPDK_NVME_CPCS_SPEC_H

#include "spdk/nvme_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Data Pointer union for NVMe commands
 * Compatible with PRP and SGL data pointers
 */
union spdk_nvme_data_ptr {
	struct {
		uint64_t prp1;
		uint64_t prp2;
	} prp;
	struct spdk_nvme_sgl_descriptor sgl1;
};

/**
 * Execute Program Command (Figure 16-22 in spec)
 */
struct spdk_nvme_cpcs_cmd_execute_program {
	/* CDW0 */
	uint16_t opc	: 8;	/* Opcode */
	uint16_t fuse	: 2;
	uint16_t rsvd	: 4;
	uint16_t psdt	: 2;
	uint16_t cid;		/* Command ID */

	/* CDW1 */
	uint32_t nsid;		/* Namespace ID */

	/* CDW2 */
	uint16_t pind;		/* Program Index */
	uint16_t rsid;		/* Memory Range Set ID */

	/* CDW3 */
	uint32_t numr;		/* Number of Memory Ranges */

	/* CDW4 */
	uint32_t dlen;		/* DPTR Data Length */

	/* CDW5 */
	uint32_t rsvd5;

	/* CDW6-9: Data Pointer */
	union spdk_nvme_data_ptr dptr;

	/* CDW10-11: CPARAM1 */
	uint64_t cparam1;

	/* CDW12-13: CPARAM2 */
	uint64_t cparam2;

	/* CDW14-15 */
	uint32_t rsvd14;
	uint32_t rsvd15;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpcs_cmd_execute_program) == 64, "Incorrect size");

/**
 * Load Program Command (Figure 39-44 in spec)
 */
struct spdk_nvme_cpcs_cmd_load_program {
	/* CDW0 */
	uint16_t opc	: 8;
	uint16_t fuse	: 2;
	uint16_t rsvd	: 4;
	uint16_t psdt	: 2;
	uint16_t cid;

	/* CDW1 */
	uint32_t nsid;

	/* CDW2-5: Reserved */
	uint32_t rsvd2;
	uint32_t rsvd3;
	uint32_t rsvd4;
	uint32_t rsvd5;

	/* CDW6-9: Data Pointer */
	union spdk_nvme_data_ptr dptr;

	/* CDW10 */
	uint16_t pind;		/* Program Index */
	uint8_t  ptype;		/* Program Type */
	uint8_t  sel	: 1;	/* Select: 0=Load, 1=Unload */
	uint8_t  pit	: 3;	/* Program Identifier Type */
	uint8_t  rsvd10	: 4;

	/* CDW11 */
	uint32_t psize;		/* Program Size */

	/* CDW12-13: Program Identifier (PID) */
	uint64_t pid;

	/* CDW14: Number of Bytes (NUMB) */
	uint32_t numb;

	/* CDW15: Load Offset (LOFF) */
	uint32_t loff;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpcs_cmd_load_program) == 64, "Incorrect size");

/**
 * Program Activation Management Command (Figure 52 in spec)
 */
struct spdk_nvme_cpcs_cmd_program_activation {
	/* CDW0 */
	uint16_t opc	: 8;
	uint16_t fuse	: 2;
	uint16_t rsvd	: 4;
	uint16_t psdt	: 2;
	uint16_t cid;

	/* CDW1 */
	uint32_t nsid;

	/* CDW2-9: Reserved/DPTR */
	uint32_t rsvd2;
	uint32_t rsvd3;
	uint32_t rsvd4;
	uint32_t rsvd5;
	uint64_t rsvd6_7;
	uint64_t rsvd8_9;

	/* CDW10 */
	uint16_t pind;		/* Program Index */
	uint8_t  sel	: 4;	/* Select: 0=Deactivate, 1=Activate */
	uint8_t  rsvd10a : 4;
	uint8_t  rsvd10b;

	/* CDW11-15 */
	uint32_t rsvd11;
	uint32_t rsvd12;
	uint32_t rsvd13;
	uint32_t rsvd14;
	uint32_t rsvd15;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpcs_cmd_program_activation) == 64, "Incorrect size");

/**
 * Memory Range Set Management Command (Figure 46-48 in spec)
 */
struct spdk_nvme_cpcs_cmd_mrs_management {
	/* CDW0 */
	uint16_t opc	: 8;
	uint16_t fuse	: 2;
	uint16_t rsvd	: 4;
	uint16_t psdt	: 2;
	uint16_t cid;

	/* CDW1 */
	uint32_t nsid;

	/* CDW2-5: Reserved */
	uint32_t rsvd2;
	uint32_t rsvd3;
	uint32_t rsvd4;
	uint32_t rsvd5;

	/* CDW6-9: Data Pointer */
	union spdk_nvme_data_ptr dptr;

	/* CDW10 */
	uint8_t  sel	: 4;	/* Select: 0=Create, 1=Delete */
	uint8_t  rsvd10a : 4;
	uint8_t  rsvd10b;
	uint16_t rsid;		/* Memory Range Set ID */

	/* CDW11 */
	uint8_t  numr;		/* Number of Memory Ranges */
	uint8_t  rsvd11[3];

	/* CDW12-15 */
	uint32_t rsvd12;
	uint32_t rsvd13;
	uint32_t rsvd14;
	uint32_t rsvd15;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpcs_cmd_mrs_management) == 64, "Incorrect size");

/**
 * Memory Range Descriptor (Figure 34 in spec)
 */
#pragma pack(push, 1)
struct spdk_nvme_cpcs_memory_range_descriptor {
	uint32_t mnsid;			/* Memory Namespace ID */
	uint32_t length;		/* Length in bytes */
	uint64_t starting_byte;		/* Starting Byte */
	uint8_t  rsvd[16];
};
#pragma pack(pop)

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpcs_memory_range_descriptor) == 32,
		   "Incorrect size");

/**
 * Program Descriptor (Figure 27 in spec)
 */
#pragma pack(push, 1)
struct spdk_nvme_cpcs_program_descriptor {
	uint8_t  peocc	: 2;	/* Program Index Occupied */
	uint8_t  act	: 1;	/* Activation */
	uint8_t  pit	: 3;	/* Program Identifier Type */
	uint8_t  rsvd0	: 2;
	uint8_t  ptype;		/* Program Type */
	uint8_t  rsvd1[6];
	uint64_t pid;		/* Program Identifier */
	uint8_t  rsvd2[48];
};
#pragma pack(pop)

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpcs_program_descriptor) == 64,
		   "Incorrect size");

/**
 * Identify Namespace - CPCS Specific (Figure 36 in spec)
 */
#pragma pack(push, 1)
struct spdk_nvme_cpcs_ns_data {
	/* Activated programs */
	uint16_t maxact;	/* Maximum Activated Programs */

	/* Memory Range Sets */
	uint16_t maxmemrs;	/* Maximum Number of Memory Range Sets */
	uint16_t mrsg;		/* Memory Range Size Granularity */
	uint8_t  maxmemr;	/* Maximum Number of Ranges in a MRS */
	uint8_t  rsvd7;

	/* Downloadable programs */
	uint64_t maxpb;		/* Maximum Bytes for All Downloaded Programs (MiB) */
	uint8_t  lpg;		/* Load Program Granularity */

	uint8_t  rsvd[4096 - 17];
};
#pragma pack(pop)

SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_cpcs_ns_data) == 4096,
		   "Incorrect size");

/**
 * Execute Program Completion (Figure 23 in spec)
 */
struct spdk_nvme_cpcs_execute_program_cpl {
	uint64_t rval;		/* Return Value */
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVME_CPCS_SPEC_H */
