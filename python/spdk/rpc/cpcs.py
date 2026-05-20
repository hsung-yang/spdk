#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Samsung Electronics Co., Ltd.
#  All rights reserved.
#


def cpcs_ns_create(client, subsystem_nqn, nsid, max_activated=None, max_mrs=None,
                   max_ranges_per_mrs=None, mrs_granularity=None,
                   max_program_bytes=None, load_program_gran=None, reach_group_id=None):
    """Create a CPCS compute namespace.
    Args:
        subsystem_nqn: NVMf subsystem NQN
        nsid: Namespace ID
        max_activated: Max concurrently activated programs
        max_mrs: Max Memory Range Sets
        max_ranges_per_mrs: Max ranges per MRS
        mrs_granularity: MRS granularity exponent
        max_program_bytes: Max total program bytes (MiB)
        load_program_gran: Load Program granularity exponent
        reach_group_id: Reachability group ID
    """
    params = {
        'subsystem_nqn': subsystem_nqn,
        'nsid': nsid,
    }
    if max_activated is not None:
        params['max_activated'] = max_activated
    if max_mrs is not None:
        params['max_mrs'] = max_mrs
    if max_ranges_per_mrs is not None:
        params['max_ranges_per_mrs'] = max_ranges_per_mrs
    if mrs_granularity is not None:
        params['mrs_granularity'] = mrs_granularity
    if max_program_bytes is not None:
        params['max_program_bytes'] = max_program_bytes
    if load_program_gran is not None:
        params['load_program_gran'] = load_program_gran
    if reach_group_id is not None:
        params['reach_group_id'] = reach_group_id
    return client.call('cpcs_ns_create', params)


def cpcs_ns_delete(client, subsystem_nqn, nsid):
    """Delete a CPCS compute namespace."""
    params = {
        'subsystem_nqn': subsystem_nqn,
        'nsid': nsid,
    }
    return client.call('cpcs_ns_delete', params)


def cpcs_program_list(client, subsystem_nqn, nsid):
    """List CPCS programs in a namespace."""
    params = {
        'subsystem_nqn': subsystem_nqn,
        'nsid': nsid,
    }
    return client.call('cpcs_program_list', params)


def cpcs_program_install_builtins(client, subsystem_nqn, nsid):
    """Install device-defined built-in CPCS programs in a namespace."""
    params = {
        'subsystem_nqn': subsystem_nqn,
        'nsid': nsid,
    }
    return client.call('cpcs_program_install_builtins', params)


def cpcs_mrs_list(client, subsystem_nqn, nsid):
    """List CPCS Memory Range Sets (MRS) in a namespace."""
    params = {
        'subsystem_nqn': subsystem_nqn,
        'nsid': nsid,
    }
    return client.call('cpcs_mrs_list', params)


def cpcs_program_install_passthrough(client, subsystem_nqn, nsid, pind):
    """Install a passthrough (ptype=0xC2) program at the specified PIND."""
    params = {
        'subsystem_nqn': subsystem_nqn,
        'nsid': nsid,
        'pind': pind,
    }
    return client.call('cpcs_program_install_passthrough', params)
