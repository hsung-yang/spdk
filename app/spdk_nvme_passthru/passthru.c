/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Samsung Electronics.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/string.h"

struct passthru_opts {
	bool admin;
	bool io;
	bool read;
	bool write;
	bool hex_dump;
	bool opcode_set;
	bool nsid_set;
	bool input_file_set;
	bool output_file_set;

	char trtype[SPDK_NVMF_TRSTRING_MAX_LEN + 1];
	char traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
	char trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1];
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];

	char input_file[PATH_MAX];
	char output_file[PATH_MAX];

	uint32_t opcode;
	uint32_t nsid;
	uint32_t data_len;

	uint32_t cdw2;
	uint32_t cdw3;
	uint64_t cdw4;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
};

static struct passthru_opts g_opts = {
	.trtype = "PCIe",
	.nsid = 1,
};

static struct spdk_nvme_ctrlr *g_ctrlr;

struct passthru_task {
	bool done;
	struct spdk_nvme_cpl cpl;
};

enum passthru_cmdline_opts {
	OPT_ADMIN = 0x1000,
	OPT_IO,
	OPT_TRTYPE,
	OPT_TRADDR,
	OPT_TRSVCID,
	OPT_SUBNQN,
	OPT_HOSTNQN,
	OPT_OPCODE,
	OPT_NSID,
	OPT_CDW2,
	OPT_CDW3,
	OPT_CDW4,
	OPT_CDW10,
	OPT_CDW11,
	OPT_CDW12,
	OPT_CDW13,
	OPT_CDW14,
	OPT_CDW15,
	OPT_DATA_LEN,
	OPT_READ,
	OPT_WRITE,
	OPT_INPUT,
	OPT_OUTPUT,
	OPT_HEX_DUMP,
};

static struct option g_cmdline_opts[] = {
	{ "admin-cmd", no_argument, NULL, OPT_ADMIN },
	{ "io-cmd", no_argument, NULL, OPT_IO },
	{ "trtype", required_argument, NULL, OPT_TRTYPE },
	{ "traddr", required_argument, NULL, OPT_TRADDR },
	{ "trsvcid", required_argument, NULL, OPT_TRSVCID },
	{ "subnqn", required_argument, NULL, OPT_SUBNQN },
	{ "hostnqn", required_argument, NULL, OPT_HOSTNQN },
	{ "opcode", required_argument, NULL, OPT_OPCODE },
	{ "nsid", required_argument, NULL, OPT_NSID },
	{ "cdw2", required_argument, NULL, OPT_CDW2 },
	{ "cdw3", required_argument, NULL, OPT_CDW3 },
	{ "cdw4", required_argument, NULL, OPT_CDW4 },
	{ "cdw10", required_argument, NULL, OPT_CDW10 },
	{ "cdw11", required_argument, NULL, OPT_CDW11 },
	{ "cdw12", required_argument, NULL, OPT_CDW12 },
	{ "cdw13", required_argument, NULL, OPT_CDW13 },
	{ "cdw14", required_argument, NULL, OPT_CDW14 },
	{ "cdw15", required_argument, NULL, OPT_CDW15 },
	{ "data-len", required_argument, NULL, OPT_DATA_LEN },
	{ "read", no_argument, NULL, OPT_READ },
	{ "write", no_argument, NULL, OPT_WRITE },
	{ "input-file", required_argument, NULL, OPT_INPUT },
	{ "output-file", required_argument, NULL, OPT_OUTPUT },
	{ "hex-dump", no_argument, NULL, OPT_HEX_DUMP },
	{ NULL, 0, NULL, 0 },
};

static void
usage(void)
{
	printf("spdk_nvme_passthru - NVMe admin/io passthru via SPDK\n");
	printf("\n");
	printf("Transport:\n");
	printf("  --trtype <PCIe|TCP|RDMA>    Transport type (default: PCIe)\n");
	printf("  --traddr <addr>             Transport address (BDF or IP)\n");
	printf("  --trsvcid <port>            Transport service id (fabrics)\n");
	printf("  --subnqn <nqn>               Subsystem NQN (fabrics)\n");
	printf("  --hostnqn <nqn>              Host NQN (fabrics)\n");
	printf("\n");
	printf("Command:\n");
	printf("  --admin-cmd                  Send admin command\n");
	printf("  --io-cmd                     Send I/O command\n");
	printf("  --opcode <num>               Opcode (required)\n");
	printf("  --nsid <num>                 Namespace ID (default: 1, admin default: 0)\n");
	printf("  --cdw2 <num>                 Command dword 2\n");
	printf("  --cdw3 <num>                 Command dword 3\n");
	printf("  --cdw4 <num>                 Command dword 4 (mptr)\n");
	printf("  --cdw10..15 <num>            Command dwords 10-15\n");
	printf("  --data-len <bytes>           Data length\n");
	printf("  --read                       Data-in (read) direction\n");
	printf("  --write                      Data-out (write) direction\n");
	printf("  --input-file <path>          Input file for write payload\n");
	printf("  --output-file <path>         Output file for read payload\n");
	printf("  --hex-dump                   Hex dump read payload to stdout\n");
}

static bool
parse_u32(const char *arg, uint32_t *val)
{
	char *end = NULL;
	unsigned long long tmp;

	if (arg == NULL || val == NULL) {
		return false;
	}

	tmp = strtoull(arg, &end, 0);
	if (end == NULL || *end != '\0' || tmp > UINT32_MAX) {
		return false;
	}

	*val = (uint32_t)tmp;
	return true;
}

static bool
parse_u64(const char *arg, uint64_t *val)
{
	char *end = NULL;
	unsigned long long tmp;

	if (arg == NULL || val == NULL) {
		return false;
	}

	tmp = strtoull(arg, &end, 0);
	if (end == NULL || *end != '\0') {
		return false;
	}

	*val = (uint64_t)tmp;
	return true;
}

static int
parse_args(int ch, char *arg)
{
	switch (ch) {
	case OPT_ADMIN:
		g_opts.admin = true;
		break;
	case OPT_IO:
		g_opts.io = true;
		break;
	case OPT_TRTYPE:
		snprintf(g_opts.trtype, sizeof(g_opts.trtype), "%s", arg);
		break;
	case OPT_TRADDR:
		snprintf(g_opts.traddr, sizeof(g_opts.traddr), "%s", arg);
		break;
	case OPT_TRSVCID:
		snprintf(g_opts.trsvcid, sizeof(g_opts.trsvcid), "%s", arg);
		break;
	case OPT_SUBNQN:
		snprintf(g_opts.subnqn, sizeof(g_opts.subnqn), "%s", arg);
		break;
	case OPT_HOSTNQN:
		snprintf(g_opts.hostnqn, sizeof(g_opts.hostnqn), "%s", arg);
		break;
	case OPT_OPCODE:
		if (!parse_u32(arg, &g_opts.opcode)) {
			return -EINVAL;
		}
		g_opts.opcode_set = true;
		break;
	case OPT_NSID:
		if (!parse_u32(arg, &g_opts.nsid)) {
			return -EINVAL;
		}
		g_opts.nsid_set = true;
		break;
	case OPT_CDW2:
		if (!parse_u32(arg, &g_opts.cdw2)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW3:
		if (!parse_u32(arg, &g_opts.cdw3)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW4:
		if (!parse_u64(arg, &g_opts.cdw4)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW10:
		if (!parse_u32(arg, &g_opts.cdw10)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW11:
		if (!parse_u32(arg, &g_opts.cdw11)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW12:
		if (!parse_u32(arg, &g_opts.cdw12)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW13:
		if (!parse_u32(arg, &g_opts.cdw13)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW14:
		if (!parse_u32(arg, &g_opts.cdw14)) {
			return -EINVAL;
		}
		break;
	case OPT_CDW15:
		if (!parse_u32(arg, &g_opts.cdw15)) {
			return -EINVAL;
		}
		break;
	case OPT_DATA_LEN:
		if (!parse_u32(arg, &g_opts.data_len)) {
			return -EINVAL;
		}
		break;
	case OPT_READ:
		g_opts.read = true;
		break;
	case OPT_WRITE:
		g_opts.write = true;
		break;
	case OPT_INPUT:
		snprintf(g_opts.input_file, sizeof(g_opts.input_file), "%s", arg);
		g_opts.input_file_set = true;
		break;
	case OPT_OUTPUT:
		snprintf(g_opts.output_file, sizeof(g_opts.output_file), "%s", arg);
		g_opts.output_file_set = true;
		break;
	case OPT_HEX_DUMP:
		g_opts.hex_dump = true;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void
hex_dump(const void *data, size_t len)
{
	const uint8_t *bytes = data;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i % 16 == 0) {
			printf("%08zx: ", i);
		}
		printf("%02x ", bytes[i]);
		if (i % 16 == 15 || i == len - 1) {
			printf("\n");
		}
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct passthru_opts *opts_cfg = cb_ctx;

	if (opts_cfg->hostnqn[0] != '\0') {
		snprintf(opts->hostnqn, sizeof(opts->hostnqn), "%s", opts_cfg->hostnqn);
	}

	return g_ctrlr == NULL;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	if (g_ctrlr == NULL) {
		g_ctrlr = ctrlr;
	} else {
		spdk_nvme_detach(ctrlr);
	}
}

static int
read_file(const char *path, void *buf, uint32_t len)
{
	int fd;
	ssize_t rc;
	uint32_t offset = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -errno;
	}

	while (offset < len) {
		rc = read(fd, (uint8_t *)buf + offset, len - offset);
		if (rc < 0) {
			close(fd);
			return -errno;
		}
		if (rc == 0) {
			break;
		}
		offset += (uint32_t)rc;
	}

	close(fd);
	if (offset != len) {
		return -EINVAL;
	}

	return 0;
}

static int
write_file(const char *path, const void *buf, uint32_t len)
{
	int fd;
	ssize_t rc;
	uint32_t offset = 0;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		return -errno;
	}

	while (offset < len) {
		rc = write(fd, (const uint8_t *)buf + offset, len - offset);
		if (rc < 0) {
			close(fd);
			return -errno;
		}
		offset += (uint32_t)rc;
	}

	close(fd);
	return 0;
}

static enum spdk_nvmf_adrfam
detect_adrfam(const char *traddr)
{
	if (traddr == NULL || traddr[0] == '\0') {
		return SPDK_NVMF_ADRFAM_IPV4;
	}
	if (strchr(traddr, ':') != NULL) {
		return SPDK_NVMF_ADRFAM_IPV6;
	}
	return SPDK_NVMF_ADRFAM_IPV4;
}

static void
cmd_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct passthru_task *task = arg;

	task->cpl = *cpl;
	task->done = true;
}

static void
passthru_run(void *arg1)
{
	struct spdk_nvme_transport_id trid = {};
	enum spdk_nvme_transport_type trtype;
	struct spdk_nvme_ctrlr_opts ctrlr_opts;
	struct spdk_nvme_qpair *qpair = NULL;
	struct spdk_nvme_cmd cmd = {};
	struct passthru_task task = {};
	void *data = NULL;
	int rc = 0;
	int64_t num_completions;

	rc = spdk_nvme_transport_id_parse_trtype(&trtype, g_opts.trtype);
	if (rc != 0) {
		fprintf(stderr, "Invalid trtype: %s\n", g_opts.trtype);
		spdk_app_stop(-EINVAL);
		return;
	}
	spdk_nvme_trid_populate_transport(&trid, trtype);

	if (g_opts.traddr[0] != '\0') {
		snprintf(trid.traddr, sizeof(trid.traddr), "%s", g_opts.traddr);
	}
	if (g_opts.trsvcid[0] != '\0') {
		snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", g_opts.trsvcid);
	}
	if (g_opts.subnqn[0] != '\0') {
		snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", g_opts.subnqn);
	}
	if (trid.traddr[0] != '\0' &&
	    (trtype == SPDK_NVME_TRANSPORT_TCP || trtype == SPDK_NVME_TRANSPORT_RDMA)) {
		trid.adrfam = detect_adrfam(trid.traddr);
	}
	fprintf(stderr, "Connecting trtype=%s adrfam=%u traddr=%s trsvcid=%s subnqn=%s\n",
		trid.trstring, trid.adrfam, trid.traddr, trid.trsvcid, trid.subnqn);
	fflush(stderr);

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr_opts, sizeof(ctrlr_opts));
	if (g_opts.hostnqn[0] != '\0') {
		snprintf(ctrlr_opts.hostnqn, sizeof(ctrlr_opts.hostnqn), "%s", g_opts.hostnqn);
	}

	if (g_opts.traddr[0] != '\0') {
		g_ctrlr = spdk_nvme_connect(&trid, &ctrlr_opts, sizeof(ctrlr_opts));
	} else if (trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		rc = spdk_nvme_probe(&trid, &g_opts, probe_cb, attach_cb, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_nvme_probe() failed\n");
		}
	} else {
		SPDK_ERRLOG("traddr is required for non-PCIe transports\n");
		rc = -EINVAL;
	}

	if (rc != 0 || g_ctrlr == NULL) {
		fprintf(stderr, "Failed to attach NVMe controller (rc=%d)\n", rc);
		spdk_app_stop(rc != 0 ? rc : -ENODEV);
		return;
	}

	if (g_opts.io) {
		qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
		if (qpair == NULL) {
			fprintf(stderr, "Failed to allocate I/O qpair\n");
			spdk_nvme_detach(g_ctrlr);
			spdk_app_stop(-ENOMEM);
			return;
		}
	}

	if (g_opts.data_len > 0) {
		data = spdk_zmalloc(g_opts.data_len, 4096, NULL,
				    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (data == NULL) {
			SPDK_ERRLOG("Failed to allocate data buffer\n");
			rc = -ENOMEM;
			goto out;
		}

		if (g_opts.write) {
			rc = read_file(g_opts.input_file, data, g_opts.data_len);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to read input file: %s\n",
					    spdk_strerror(-rc));
				goto out;
			}
		} else {
			memset(data, 0, g_opts.data_len);
		}
	}

	cmd.opc = (uint8_t)g_opts.opcode;
	cmd.nsid = g_opts.nsid;
	cmd.rsvd2 = g_opts.cdw2;
	cmd.rsvd3 = g_opts.cdw3;
	cmd.mptr = g_opts.cdw4;
	cmd.cdw10 = g_opts.cdw10;
	cmd.cdw11 = g_opts.cdw11;
	cmd.cdw12 = g_opts.cdw12;
	cmd.cdw13 = g_opts.cdw13;
	cmd.cdw14 = g_opts.cdw14;
	cmd.cdw15 = g_opts.cdw15;

	if (g_opts.admin) {
		rc = spdk_nvme_ctrlr_cmd_admin_raw(g_ctrlr, &cmd, data, g_opts.data_len,
						   cmd_complete, &task);
	} else {
		rc = spdk_nvme_ctrlr_cmd_io_raw(g_ctrlr, qpair, &cmd, data, g_opts.data_len,
						cmd_complete, &task);
	}

	if (rc != 0) {
		fprintf(stderr, "Command submission failed: %s\n", spdk_strerror(-rc));
		goto out;
	}

	while (!task.done) {
		if (g_opts.admin) {
			num_completions = spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
		} else {
			num_completions = spdk_nvme_qpair_process_completions(qpair, 0);
		}

		if (num_completions < 0) {
			fprintf(stderr, "Failed processing completions: %s\n",
				spdk_strerror((int)-num_completions));
			rc = (int)num_completions;
			goto out;
		}
	}

	if (spdk_nvme_cpl_is_error(&task.cpl)) {
		const struct spdk_nvme_status *status = &task.cpl.status;
		fprintf(stderr, "Command failed: sct=0x%x sc=0x%x (%s: %s)\n",
			status->sct, status->sc,
			spdk_nvme_cpl_get_status_type_string(status),
			spdk_nvme_cpl_get_status_string(status));
		rc = -EIO;
	} else {
		printf("Command completed: result=0x%08x\n", task.cpl.cdw0);
	}

	if (rc == 0 && g_opts.read && g_opts.data_len > 0) {
		if (g_opts.output_file_set) {
			rc = write_file(g_opts.output_file, data, g_opts.data_len);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to write output file: %s\n",
					    spdk_strerror(-rc));
			}
		} else if (g_opts.hex_dump) {
			hex_dump(data, g_opts.data_len);
		}
	}

out:
	if (data) {
		spdk_free(data);
	}
	if (qpair) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}
	if (g_ctrlr) {
		spdk_nvme_detach(g_ctrlr);
	}
	spdk_app_stop(rc);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "spdk_nvme_passthru";

	rc = spdk_app_parse_args(argc, argv, &opts, "", g_cmdline_opts, parse_args, usage);
	if (rc == SPDK_APP_PARSE_ARGS_FAIL) {
		SPDK_ERRLOG("Invalid arguments\n");
		return rc;
	} else if (rc == SPDK_APP_PARSE_ARGS_HELP) {
		return 0;
	}

	if (g_opts.admin == g_opts.io) {
		SPDK_ERRLOG("Specify exactly one of --admin-cmd or --io-cmd\n");
		return -EINVAL;
	}
	if (!g_opts.opcode_set) {
		SPDK_ERRLOG("--opcode is required\n");
		return -EINVAL;
	}
	if (!g_opts.nsid_set && g_opts.admin) {
		g_opts.nsid = 0;
	}
	if (g_opts.read && g_opts.write) {
		SPDK_ERRLOG("Specify only one of --read or --write\n");
		return -EINVAL;
	}
	if (g_opts.data_len > 0 && !(g_opts.read || g_opts.write)) {
		SPDK_ERRLOG("--data-len requires --read or --write\n");
		return -EINVAL;
	}
	if (g_opts.write && !g_opts.input_file_set && g_opts.data_len > 0) {
		SPDK_ERRLOG("--write requires --input-file\n");
		return -EINVAL;
	}
	if (g_opts.input_file_set && g_opts.data_len == 0) {
		SPDK_ERRLOG("--input-file requires --data-len\n");
		return -EINVAL;
	}
	if (g_opts.read && !g_opts.output_file_set) {
		g_opts.hex_dump = true;
	}

	if (g_opts.trtype[0] == '\0') {
		snprintf(g_opts.trtype, sizeof(g_opts.trtype), "%s", "PCIe");
	}
	if (strcasecmp(g_opts.trtype, "pcie") != 0) {
		opts.no_pci = true;
	}

	rc = spdk_app_start(&opts, passthru_run, NULL);
	if (rc) {
		fprintf(stderr, "spdk_app_start failed (rc=%d)\n", rc);
		SPDK_ERRLOG("Application error: %s\n", spdk_strerror(-rc));
	}

	spdk_app_fini();
	return rc;
}
