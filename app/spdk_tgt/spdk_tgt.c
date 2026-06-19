/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "nvmf/cpcs/builtin_runtime.h"
#if defined(SPDK_CONFIG_VHOST)
#include "spdk/vhost.h"
#endif
#if defined(SPDK_CONFIG_VFIO_USER)
#include "spdk/vfu_target.h"
#endif

#if defined(SPDK_CONFIG_VHOST) || defined(SPDK_CONFIG_VFIO_USER)
#define SPDK_SOCK_PATH "S:"
#else
#define SPDK_SOCK_PATH
#endif

static const char *g_pid_path = NULL;
static const char *g_cpcs_compute_core_mask_arg = NULL;
static const char g_spdk_tgt_get_opts_string[] = "f:" SPDK_SOCK_PATH;

enum spdk_tgt_cmdline_opts {
	CPCS_COMPUTE_CORE_MASK_OPT_IDX = 1000,
};

static const struct option g_spdk_tgt_cmdline_options[] = {
	{"cpcs-compute-core-mask", required_argument, NULL, CPCS_COMPUTE_CORE_MASK_OPT_IDX},
	{},
};

static void
spdk_tgt_usage(void)
{
	printf(" -f <file>                 pidfile save pid to file under given path\n");
	printf(" --cpcs-compute-core-mask <mask or list>\n");
	printf("                           dedicate these app cores to CPCS builtin compute work\n");
#if defined(SPDK_CONFIG_VHOST) || defined(SPDK_CONFIG_VFIO_USER)
	printf(" -S <path>                 directory where to create vhost/vfio-user sockets (default: pwd)\n");
#endif
}

static void
spdk_tgt_save_pid(const char *pid_path)
{
	FILE *pid_file;

	pid_file = fopen(pid_path, "w");
	if (pid_file == NULL) {
		fprintf(stderr, "Couldn't create pid file '%s': %s\n", pid_path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	fprintf(pid_file, "%d\n", getpid());
	fclose(pid_file);
}

static void
spdk_tgt_set_cpcs_compute_core_mask_arg(char *arg)
{
	g_cpcs_compute_core_mask_arg = arg;
}

static int
spdk_tgt_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_pid_path = arg;
		break;
	case CPCS_COMPUTE_CORE_MASK_OPT_IDX:
		spdk_tgt_set_cpcs_compute_core_mask_arg(arg);
		break;
#if defined(SPDK_CONFIG_VHOST) || defined(SPDK_CONFIG_VFIO_USER)
	case 'S':
#ifdef SPDK_CONFIG_VHOST
		spdk_vhost_set_socket_path(arg);
#endif
#ifdef SPDK_CONFIG_VFIO_USER
		spdk_vfu_set_socket_path(arg);
#endif
		break;
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

static int
spdk_tgt_validate_cpcs_compute_core_mask(const struct spdk_cpuset *compute_mask,
		const struct spdk_cpuset *app_mask)
{
	struct spdk_cpuset app_mask_copy = {};
	struct spdk_cpuset configured_mask = {};
	struct spdk_cpuset valid_mask = {};
	struct spdk_cpuset non_compute_mask = {};

	if (compute_mask == NULL || app_mask == NULL) {
		return -EINVAL;
	}

	spdk_cpuset_copy(&app_mask_copy, app_mask);
	spdk_cpuset_copy(&configured_mask, compute_mask);
	spdk_cpuset_copy(&valid_mask, compute_mask);
	spdk_cpuset_and(&valid_mask, &app_mask_copy);
	if (!spdk_cpuset_equal(&valid_mask, &configured_mask)) {
		SPDK_ERRLOG("CPCS compute core mask 0x%s must be a subset of app core mask 0x%s\n",
			    spdk_cpuset_fmt(&configured_mask), spdk_cpuset_fmt(&app_mask_copy));
		return -EINVAL;
	}

	spdk_cpuset_copy(&non_compute_mask, &configured_mask);
	spdk_cpuset_negate(&non_compute_mask);
	spdk_cpuset_and(&non_compute_mask, &app_mask_copy);
	if (spdk_cpuset_count(&non_compute_mask) == 0) {
		SPDK_ERRLOG("CPCS compute core mask 0x%s leaves no app cores for NVMf poll groups\n",
			    spdk_cpuset_fmt(&configured_mask));
		return -EINVAL;
	}

	return 0;
}

static int
spdk_tgt_configure_cpcs_compute_cores(struct spdk_app_opts *opts)
{
	struct spdk_cpuset app_mask = {};
	struct spdk_cpuset compute_mask = {};
	const char *reactor_mask;
	int rc;

	if (g_cpcs_compute_core_mask_arg == NULL) {
		return 0;
	}

	rc = spdk_cpuset_parse(&compute_mask, g_cpcs_compute_core_mask_arg);
	if (rc != 0 || spdk_cpuset_count(&compute_mask) == 0) {
		fprintf(stderr, "Invalid --cpcs-compute-core-mask '%s'\n",
			g_cpcs_compute_core_mask_arg);
		return -EINVAL;
	}

	if (opts != NULL && opts->lcore_map == NULL) {
		reactor_mask = opts->reactor_mask != NULL ? opts->reactor_mask : "0x1";
		rc = spdk_cpuset_parse(&app_mask, reactor_mask);
		if (rc != 0) {
			return rc;
		}

		rc = spdk_tgt_validate_cpcs_compute_core_mask(&compute_mask, &app_mask);
		if (rc != 0) {
			return rc;
		}
	}

	return cpcs_builtin_runtime_set_compute_core_mask(&compute_mask);
}

static int
spdk_tgt_validate_cpcs_compute_cores(void)
{
	const struct spdk_cpuset *compute_mask;
	const struct spdk_cpuset *app_mask;

	compute_mask = cpcs_builtin_runtime_get_compute_core_mask();
	if (compute_mask == NULL) {
		return 0;
	}

	app_mask = spdk_app_get_core_mask();
	return spdk_tgt_validate_cpcs_compute_core_mask(compute_mask, app_mask);
}

static void
spdk_tgt_shutdown(void)
{
	cpcs_builtin_runtime_stop_compute_threads();
	spdk_app_stop(0);
}

static void
spdk_tgt_started(void *arg1)
{
	int rc;

	(void)arg1;

	rc = spdk_tgt_validate_cpcs_compute_cores();
	if (rc != 0) {
		spdk_app_stop(rc);
		return;
	}

	rc = cpcs_builtin_runtime_start_compute_threads();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to start CPCS compute threads: %d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	if (g_pid_path) {
		spdk_tgt_save_pid(g_pid_path);
	}

	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "spdk_tgt";
	opts.shutdown_cb = spdk_tgt_shutdown;
	if ((rc = spdk_app_parse_args(argc, argv, &opts, g_spdk_tgt_get_opts_string,
				      g_spdk_tgt_cmdline_options, spdk_tgt_parse_arg, spdk_tgt_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_tgt_configure_cpcs_compute_cores(&opts);
	if (rc != 0) {
		return rc;
	}

	rc = spdk_app_start(&opts, spdk_tgt_started, NULL);
	spdk_app_fini();

	return rc;
}
