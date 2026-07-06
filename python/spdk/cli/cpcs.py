#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Samsung Electronics Co., Ltd.
#  All rights reserved.
#

from spdk.rpc.cmd_parser import strip_globals, remove_null
from spdk.rpc.client import print_json  # noqa


def add_parser(subparsers):

    def cpcs_ns_create(args):
        params = remove_null(strip_globals(vars(args)))
        print_json(args.client.cpcs_ns_create(**params))

    p = subparsers.add_parser('cpcs_ns_create', help='Create a CPCS compute namespace')
    p.add_argument('--subsystem-nqn', dest='subsystem_nqn', required=True, help='NVMf subsystem NQN')
    p.add_argument('--nsid', dest='nsid', required=True, type=int, help='Namespace ID')
    p.add_argument('--max-activated', dest='max_activated', type=int, help='Max concurrently activated programs')
    p.add_argument('--max-mrs', dest='max_mrs', type=int, help='Max Memory Range Sets (MAXMEMRS)')
    p.add_argument('--max-ranges-per-mrs', dest='max_ranges_per_mrs', type=int,
                   help='Max ranges per MRS (MAXMEMR)')
    p.add_argument('--mrs-granularity', dest='mrs_granularity', type=int, help='MRS granularity exponent (MRSG)')
    p.add_argument('--max-program-bytes', dest='max_program_bytes', type=int, help='Max total program bytes (MiB)')
    p.add_argument('--load-program-gran', dest='load_program_gran', type=int,
                   help='Load Program granularity exponent (LPG)')
    p.add_argument('--reach-group-id', dest='reach_group_id', type=int, help='Reachability group ID')
    p.set_defaults(func=cpcs_ns_create)

    def cpcs_ns_delete(args):
        params = remove_null(strip_globals(vars(args)))
        print_json(args.client.cpcs_ns_delete(**params))

    p = subparsers.add_parser('cpcs_ns_delete', help='Delete a CPCS compute namespace')
    p.add_argument('--subsystem-nqn', dest='subsystem_nqn', required=True, help='NVMf subsystem NQN')
    p.add_argument('--nsid', dest='nsid', required=True, type=int, help='Namespace ID')
    p.set_defaults(func=cpcs_ns_delete)

    def cpcs_program_list(args):
        params = remove_null(strip_globals(vars(args)))
        print_json(args.client.cpcs_program_list(**params))

    p = subparsers.add_parser('cpcs_program_list', help='List CPCS programs in a namespace')
    p.add_argument('--subsystem-nqn', dest='subsystem_nqn', required=True, help='NVMf subsystem NQN')
    p.add_argument('--nsid', dest='nsid', required=True, type=int, help='Namespace ID')
    p.set_defaults(func=cpcs_program_list)

    def cpcs_program_install_builtins(args):
        params = remove_null(strip_globals(vars(args)))
        print_json(args.client.cpcs_program_install_builtins(**params))

    p = subparsers.add_parser('cpcs_program_install_builtins',
                              help='Install device-defined built-in programs in a namespace')
    p.add_argument('--subsystem-nqn', dest='subsystem_nqn', required=True, help='NVMf subsystem NQN')
    p.add_argument('--nsid', dest='nsid', required=True, type=int, help='Namespace ID')
    p.set_defaults(func=cpcs_program_install_builtins)

    def cpcs_mrs_list(args):
        params = remove_null(strip_globals(vars(args)))
        print_json(args.client.cpcs_mrs_list(**params))

    p = subparsers.add_parser('cpcs_mrs_list', help='List CPCS Memory Range Sets (MRS) in a namespace')
    p.add_argument('--subsystem-nqn', dest='subsystem_nqn', required=True, help='NVMf subsystem NQN')
    p.add_argument('--nsid', dest='nsid', required=True, type=int, help='Namespace ID')
    p.set_defaults(func=cpcs_mrs_list)

    def cpcs_program_install_passthrough(args):
        params = remove_null(strip_globals(vars(args)))
        print_json(args.client.cpcs_program_install_passthrough(**params))

    p = subparsers.add_parser('cpcs_program_install_passthrough',
                              help='Install a passthrough (ptype=0xC2) program at a PIND')
    p.add_argument('--subsystem-nqn', dest='subsystem_nqn', required=True, help='NVMf subsystem NQN')
    p.add_argument('--nsid', dest='nsid', required=True, type=int, help='Namespace ID')
    p.add_argument('--pind', dest='pind', required=True, type=int, help='Program Index (PIND)')
    p.set_defaults(func=cpcs_program_install_passthrough)
