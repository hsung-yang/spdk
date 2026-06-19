#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
from typing import Any, Dict, List, Optional, Sequence

DEFAULT_TARGET_LOG = "/var/log/spdk.log"


class TeeStream:
    def __init__(self, primary: io.TextIOBase, mirror: io.TextIOBase):
        self._primary = primary
        self._mirror = mirror

    def write(self, text: str) -> int:
        self._primary.write(text)
        self._mirror.write(text)
        return len(text)

    def flush(self) -> None:
        self._primary.flush()
        self._mirror.flush()

    def isatty(self) -> bool:
        try:
            return self._primary.isatty()
        except Exception:
            return False

    @property
    def encoding(self) -> str:
        return getattr(self._primary, "encoding", "utf-8")


@contextmanager
def tee_output_python_streams(log_path: Path):
    with log_path.open("a", encoding="utf-8") as mirror:
        stdout_tee = TeeStream(sys.stdout, mirror)
        stderr_tee = TeeStream(sys.stderr, mirror)
        old_stdout = sys.stdout
        old_stderr = sys.stderr
        sys.stdout = stdout_tee  # type: ignore[assignment]
        sys.stderr = stderr_tee  # type: ignore[assignment]
        try:
            yield
        finally:
            sys.stdout = old_stdout
            sys.stderr = old_stderr


@contextmanager
def tee_output(log_path: Optional[Path]):
    if log_path is None:
        yield
        return

    log_path.parent.mkdir(parents=True, exist_ok=True)

    try:
        stdout_fd = sys.stdout.fileno()
        stderr_fd = sys.stderr.fileno()
    except (AttributeError, io.UnsupportedOperation, OSError):
        with tee_output_python_streams(log_path):
            yield
        return

    sys.stdout.flush()
    sys.stderr.flush()
    saved_stdout = os.dup(stdout_fd)
    saved_stderr = os.dup(stderr_fd)
    try:
        tee = subprocess.Popen(
            ["tee", "-a", str(log_path)],
            stdin=subprocess.PIPE,
            stdout=saved_stdout,
            stderr=saved_stderr,
        )
    except OSError:
        os.close(saved_stdout)
        os.close(saved_stderr)
        with tee_output_python_streams(log_path):
            yield
        return

    try:
        if tee.stdin is None:
            raise RuntimeError("failed to initialize tee stdin")
        tee_fd = tee.stdin.fileno()
        os.dup2(tee_fd, stdout_fd)
        os.dup2(tee_fd, stderr_fd)
        yield
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
        os.dup2(saved_stdout, stdout_fd)
        os.dup2(saved_stderr, stderr_fd)
        os.close(saved_stdout)
        os.close(saved_stderr)
        if tee.stdin is not None:
            tee.stdin.close()
        tee.wait()


@dataclass
class HostSpec:
    name: str
    mode: str
    repo_path: str
    ssh_host: Optional[str]
    ssh_user: Optional[str]
    ssh_port: int
    ssh_options: Sequence[str]
    use_sudo: bool


@dataclass
class PathSpec:
    rpc_py: str
    spdk_tgt: str
    spdk_nvme_passthru: str


@dataclass
class NvmeofSpec:
    trtype: str
    traddr: str
    trsvcid: str
    nqn: str
    hostnqn: str
    src_addr: Optional[str]
    src_svcid: Optional[str]
    pcie_bdf: Optional[str]
    dataset_bdev: Optional[str]
    backing_bdev: Optional[str]


@dataclass
class RuntimeSpec:
    target_core_mask: Optional[str]
    target_compute_core_mask: Optional[str]
    initiator_passthru_lcores: Optional[str]
    vector_prefill_workers: Optional[int]
    direct_probe_nsid: Optional[int]
    direct_probe_lba_bytes: Optional[int]


@dataclass
class Inventory:
    hosts: Dict[str, HostSpec]
    initiator_role: str
    target_role: str
    paths: PathSpec
    nvmeof: NvmeofSpec
    runtime: RuntimeSpec


class Runner:
    def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        raise NotImplementedError

    def run_shell(
        self,
        command: str,
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        raise NotImplementedError


class LocalRunner(Runner):
    def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        full_cmd = list(cmd)
        if sudo:
            full_cmd = ["sudo", "-n", *full_cmd]
        if log_cmd:
            print(f"+ {' '.join(shlex.quote(token) for token in full_cmd)}")
        cp = subprocess.run(full_cmd, cwd=cwd, text=True, capture_output=capture_output)
        if check and cp.returncode != 0:
            raise RuntimeError(
                f"Command failed (rc={cp.returncode})\n"
                f"cmd: {' '.join(shlex.quote(token) for token in full_cmd)}\n"
                f"stdout:\n{cp.stdout}\n"
                f"stderr:\n{cp.stderr}"
            )
        return cp

    def run_shell(
        self,
        command: str,
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        cmd = command
        if cwd:
            cmd = f"cd {shlex.quote(cwd)} && {cmd}"
        if sudo:
            cmd = f"sudo -n sh -c {shlex.quote(cmd)}"
        full_cmd = ["bash", "-lc", cmd]
        if log_cmd:
            print(f"+ {' '.join(shlex.quote(token) for token in full_cmd)}")
        cp = subprocess.run(full_cmd, text=True, capture_output=capture_output)
        if check and cp.returncode != 0:
            raise RuntimeError(
                f"Command failed (rc={cp.returncode})\n"
                f"cmd: {' '.join(shlex.quote(token) for token in full_cmd)}\n"
                f"stdout:\n{cp.stdout}\n"
                f"stderr:\n{cp.stderr}"
            )
        return cp


class SshRunner(Runner):
    def __init__(self, ssh_host: str, ssh_user: Optional[str], ssh_port: int, ssh_options: Sequence[str]):
        self.ssh_host = ssh_host
        self.ssh_user = ssh_user
        self.ssh_port = ssh_port
        self.ssh_options = list(ssh_options)

    def _prefix(self) -> List[str]:
        dest = f"{self.ssh_user}@{self.ssh_host}" if self.ssh_user else self.ssh_host
        cmd = ["ssh", "-p", str(self.ssh_port)]
        for opt in self.ssh_options:
            cmd.extend(["-o", opt])
        cmd.append(dest)
        return cmd

    def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        command = " ".join(shlex.quote(token) for token in cmd)
        return self.run_shell(
            command,
            cwd=cwd,
            check=check,
            capture_output=capture_output,
            sudo=sudo,
            log_cmd=log_cmd,
        )

    def run_shell(
        self,
        command: str,
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        remote = command
        if cwd:
            remote = f"cd {shlex.quote(cwd)} && {remote}"
        if sudo:
            remote = f"sudo -n sh -c {shlex.quote(remote)}"
        full_cmd = self._prefix() + [remote]
        if log_cmd:
            print(f"+ {' '.join(shlex.quote(token) for token in full_cmd)}")
        cp = subprocess.run(full_cmd, text=True, capture_output=capture_output)
        if check and cp.returncode != 0:
            raise RuntimeError(
                f"Command failed (rc={cp.returncode})\n"
                f"cmd: {' '.join(shlex.quote(token) for token in full_cmd)}\n"
                f"stdout:\n{cp.stdout}\n"
                f"stderr:\n{cp.stderr}"
            )
        return cp


class HostContext:
    def __init__(self, spec: HostSpec):
        self.spec = spec
        if spec.mode == "local":
            self.runner: Runner = LocalRunner()
        else:
            self.runner = SshRunner(
                ssh_host=spec.ssh_host or "",
                ssh_user=spec.ssh_user,
                ssh_port=spec.ssh_port,
                ssh_options=spec.ssh_options,
            )

    def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        return self.runner.run(
            cmd,
            cwd=cwd,
            check=check,
            capture_output=capture_output,
            sudo=sudo,
            log_cmd=log_cmd,
        )

    def run_shell(
        self,
        command: str,
        *,
        cwd: Optional[str] = None,
        check: bool = True,
        capture_output: bool = True,
        sudo: bool = False,
        log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        return self.runner.run_shell(
            command,
            cwd=cwd,
            check=check,
            capture_output=capture_output,
            sudo=sudo,
            log_cmd=log_cmd,
        )

    def run_json(self, cmd: Sequence[str], *, cwd: Optional[str] = None, sudo: bool = False) -> Any:
        cp = self.run(cmd, cwd=cwd, sudo=sudo, check=True, capture_output=True)
        try:
            return json.loads(cp.stdout or "")
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"Failed to parse JSON output from host {self.spec.name}:\n{cp.stdout}") from exc

    def sudo_run(self, cmd: Sequence[str], *, cwd: Optional[str] = None, check: bool = True) -> subprocess.CompletedProcess:
        return self.run(cmd, cwd=cwd, sudo=True, check=check, capture_output=True)

    def exists(self, path: str) -> bool:
        cp = self.run_shell(f"test -e {shlex.quote(path)}", check=False, capture_output=True, log_cmd=False)
        return cp.returncode == 0


@dataclass
class PlatformContext:
    inventory: Inventory
    initiator: HostContext
    target: HostContext
    artifacts_root: Path
    run_id: str
    git_commit: str
    repo_root: Path

    @property
    def split_mode(self) -> bool:
        return self.inventory.initiator_role != self.inventory.target_role


def _require_field(obj: Dict[str, Any], key: str) -> Any:
    if key not in obj:
        raise ValueError(f"Missing required field: {key}")
    return obj[key]


def _resolve_host(name: str, raw: Dict[str, Any]) -> HostSpec:
    mode = str(_require_field(raw, "mode"))
    if mode not in ("local", "ssh"):
        raise ValueError(f"hosts.{name}.mode must be 'local' or 'ssh'")

    repo_path = str(_require_field(raw, "repo_path"))
    ssh_host = raw.get("ssh_host")
    ssh_user = raw.get("ssh_user")
    ssh_port = int(raw.get("ssh_port", 22))
    ssh_options = raw.get("ssh_options", [])
    use_sudo = bool(raw.get("use_sudo", True))

    if mode == "ssh" and not ssh_host:
        raise ValueError(f"hosts.{name}.ssh_host is required when mode=ssh")
    if not isinstance(ssh_options, list):
        raise ValueError(f"hosts.{name}.ssh_options must be a list")

    return HostSpec(
        name=name,
        mode=mode,
        repo_path=repo_path,
        ssh_host=str(ssh_host) if ssh_host is not None else None,
        ssh_user=str(ssh_user) if ssh_user is not None else None,
        ssh_port=ssh_port,
        ssh_options=[str(entry) for entry in ssh_options],
        use_sudo=use_sudo,
    )


def _strip_yaml_comment(line: str) -> str:
    out: List[str] = []
    in_single = False
    in_double = False
    for ch in line:
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif ch == "#" and not in_single and not in_double:
            break
        out.append(ch)
    return "".join(out).rstrip()


def _parse_yaml_scalar(token: str) -> Any:
    text = token.strip()
    if text == "":
        return ""
    if len(text) >= 2 and ((text[0] == "'" and text[-1] == "'") or (text[0] == '"' and text[-1] == '"')):
        return text[1:-1]

    lowered = text.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if lowered in ("null", "none", "~"):
        return None

    try:
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(text)
    except ValueError:
        pass

    try:
        return float(text)
    except ValueError:
        return text


def _parse_simple_yaml(text: str) -> Dict[str, Any]:
    lines: List[tuple[int, str]] = []
    for raw in text.splitlines():
        no_comment = _strip_yaml_comment(raw)
        if not no_comment.strip():
            continue
        indent = len(no_comment) - len(no_comment.lstrip(" "))
        lines.append((indent, no_comment.strip()))

    if not lines:
        return {}

    def parse_node(idx: int, indent: int) -> tuple[Any, int]:
        if idx >= len(lines):
            return {}, idx
        current_indent, current_text = lines[idx]
        if current_indent != indent:
            raise ValueError(f"invalid indentation near: {current_text}")
        if current_text.startswith("- "):
            return parse_list(idx, indent)
        return parse_map(idx, indent)

    def parse_map(idx: int, indent: int) -> tuple[Dict[str, Any], int]:
        out: Dict[str, Any] = {}
        while idx < len(lines):
            current_indent, current_text = lines[idx]
            if current_indent < indent:
                break
            if current_indent > indent:
                raise ValueError(f"unexpected indentation near: {current_text}")
            if current_text.startswith("- "):
                break

            key, sep, remainder = current_text.partition(":")
            if sep == "":
                raise ValueError(f"invalid YAML mapping line: {current_text}")

            key = key.strip()
            remainder = remainder.strip()
            idx += 1
            if remainder != "":
                out[key] = _parse_yaml_scalar(remainder)
                continue

            if idx < len(lines) and lines[idx][0] > current_indent:
                child_indent = lines[idx][0]
                child_value, idx = parse_node(idx, child_indent)
                out[key] = child_value
            else:
                out[key] = {}
        return out, idx

    def parse_list(idx: int, indent: int) -> tuple[List[Any], int]:
        out: List[Any] = []
        while idx < len(lines):
            current_indent, current_text = lines[idx]
            if current_indent < indent:
                break
            if current_indent > indent:
                raise ValueError(f"unexpected indentation near: {current_text}")
            if not current_text.startswith("- "):
                break

            remainder = current_text[2:].strip()
            idx += 1
            if remainder != "":
                out.append(_parse_yaml_scalar(remainder))
                continue

            if idx < len(lines) and lines[idx][0] > current_indent:
                child_indent = lines[idx][0]
                child_value, idx = parse_node(idx, child_indent)
                out.append(child_value)
            else:
                out.append(None)
        return out, idx

    root, next_idx = parse_node(0, lines[0][0])
    if next_idx != len(lines):
        raise ValueError("trailing unparsed YAML content")
    if not isinstance(root, dict):
        raise ValueError("Inventory root must be a mapping")
    return root


def _load_inventory_raw(inv_path: Path) -> Dict[str, Any]:
    text = inv_path.read_text(encoding="utf-8")
    try:
        import yaml  # type: ignore
    except Exception:
        raw = _parse_simple_yaml(text)
    else:
        raw = yaml.safe_load(text)
        if raw is None:
            raw = {}
    if not isinstance(raw, dict):
        raise ValueError("Inventory root must be a YAML mapping")
    return raw


def load_inventory(path: str) -> Inventory:
    inv_path = Path(path)
    if not inv_path.is_file():
        raise FileNotFoundError(f"Inventory file not found: {path}")

    raw = _load_inventory_raw(inv_path)

    raw_hosts = _require_field(raw, "hosts")
    if not isinstance(raw_hosts, dict) or not raw_hosts:
        raise ValueError("inventory hosts must be a non-empty mapping")

    hosts: Dict[str, HostSpec] = {}
    for name, entry in raw_hosts.items():
        if not isinstance(entry, dict):
            raise ValueError(f"hosts.{name} must be a mapping")
        hosts[name] = _resolve_host(str(name), entry)

    raw_roles = _require_field(raw, "roles")
    if not isinstance(raw_roles, dict):
        raise ValueError("roles must be a mapping")
    initiator_role = str(_require_field(raw_roles, "initiator"))
    target_role = str(_require_field(raw_roles, "target"))
    if initiator_role not in hosts:
        raise ValueError(f"roles.initiator references unknown host: {initiator_role}")
    if target_role not in hosts:
        raise ValueError(f"roles.target references unknown host: {target_role}")

    raw_paths = raw.get("paths", {})
    if not isinstance(raw_paths, dict):
        raise ValueError("paths must be a mapping")
    paths = PathSpec(
        rpc_py=str(raw_paths.get("rpc_py", "scripts/rpc.py")),
        spdk_tgt=str(raw_paths.get("spdk_tgt", "build/bin/spdk_tgt")),
        spdk_nvme_passthru=str(raw_paths.get("spdk_nvme_passthru", "build/bin/spdk_nvme_passthru")),
    )

    raw_nvmeof = _require_field(raw, "nvmeof")
    if not isinstance(raw_nvmeof, dict):
        raise ValueError("nvmeof must be a mapping")
    nvmeof = NvmeofSpec(
        trtype=str(_require_field(raw_nvmeof, "trtype")),
        traddr=str(_require_field(raw_nvmeof, "traddr")),
        trsvcid=str(_require_field(raw_nvmeof, "trsvcid")),
        nqn=str(_require_field(raw_nvmeof, "nqn")),
        hostnqn=str(_require_field(raw_nvmeof, "hostnqn")),
        src_addr=(str(raw_nvmeof.get("src_addr")) if raw_nvmeof.get("src_addr") is not None else None),
        src_svcid=(str(raw_nvmeof.get("src_svcid")) if raw_nvmeof.get("src_svcid") is not None else None),
        pcie_bdf=(str(raw_nvmeof.get("pcie_bdf")) if raw_nvmeof.get("pcie_bdf") is not None else None),
        dataset_bdev=(str(raw_nvmeof.get("dataset_bdev")) if raw_nvmeof.get("dataset_bdev") is not None else None),
        backing_bdev=(str(raw_nvmeof.get("backing_bdev")) if raw_nvmeof.get("backing_bdev") is not None else None),
    )

    raw_runtime = raw.get("runtime", {})
    if not isinstance(raw_runtime, dict):
        raise ValueError("runtime must be a mapping")
    target_core_mask = raw_runtime.get("target_core_mask")
    target_compute_core_mask = raw_runtime.get("target_compute_core_mask")
    initiator_passthru_lcores = raw_runtime.get("initiator_passthru_lcores")
    vector_prefill_workers_raw = raw_runtime.get("vector_prefill_workers")
    direct_probe_nsid_raw = raw_runtime.get("direct_probe_nsid")
    direct_probe_lba_bytes_raw = raw_runtime.get("direct_probe_lba_bytes")
    vector_prefill_workers: Optional[int] = None
    direct_probe_nsid: Optional[int] = None
    direct_probe_lba_bytes: Optional[int] = None
    if vector_prefill_workers_raw is not None:
        vector_prefill_workers = int(vector_prefill_workers_raw)
        if vector_prefill_workers < 0:
            raise ValueError("runtime.vector_prefill_workers must be >= 0")
    if direct_probe_nsid_raw is not None:
        direct_probe_nsid = int(direct_probe_nsid_raw)
        if direct_probe_nsid <= 0:
            raise ValueError("runtime.direct_probe_nsid must be > 0")
    if direct_probe_lba_bytes_raw is not None:
        direct_probe_lba_bytes = int(direct_probe_lba_bytes_raw)
        if direct_probe_lba_bytes <= 0:
            raise ValueError("runtime.direct_probe_lba_bytes must be > 0")

    runtime = RuntimeSpec(
        target_core_mask=(str(target_core_mask).strip() if target_core_mask is not None else None),
        target_compute_core_mask=(
            str(target_compute_core_mask).strip()
            if target_compute_core_mask is not None
            else None
        ),
        initiator_passthru_lcores=(
            str(initiator_passthru_lcores).strip()
            if initiator_passthru_lcores is not None
            else None
        ),
        vector_prefill_workers=vector_prefill_workers,
        direct_probe_nsid=direct_probe_nsid,
        direct_probe_lba_bytes=direct_probe_lba_bytes,
    )
    if runtime.target_core_mask == "":
        runtime.target_core_mask = None
    if runtime.target_compute_core_mask == "":
        runtime.target_compute_core_mask = None
    if runtime.initiator_passthru_lcores == "":
        runtime.initiator_passthru_lcores = None

    return Inventory(
        hosts=hosts,
        initiator_role=initiator_role,
        target_role=target_role,
        paths=paths,
        nvmeof=nvmeof,
        runtime=runtime,
    )


def resolve_bin(repo_path: str, rel_or_abs: str) -> str:
    p = Path(rel_or_abs)
    if p.is_absolute():
        return str(p)
    return str(Path(repo_path) / p)


def resolve_repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def resolve_git_commit(repo_root: Path) -> str:
    try:
        cp = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "--short", "HEAD"],
            check=True,
            text=True,
            capture_output=True,
        )
        return (cp.stdout or "").strip() or "unknown"
    except Exception:
        return "unknown"


def build_platform_context(inventory: Inventory, artifacts_root: Optional[str]) -> PlatformContext:
    initiator = HostContext(inventory.hosts[inventory.initiator_role])
    target = HostContext(inventory.hosts[inventory.target_role])

    if initiator.spec.mode != "local":
        raise ValueError(
            "roles.initiator must use mode=local so spdk_nvme_passthru runs on the orchestrator host"
        )

    if artifacts_root:
        root = Path(artifacts_root)
    else:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        root = Path("/tmp/cpcs_experiments") / ts
    root.mkdir(parents=True, exist_ok=True)

    run_id = datetime.now().strftime("run_%Y%m%d_%H%M%S")
    repo_root = resolve_repo_root()
    git_commit = resolve_git_commit(repo_root)

    return PlatformContext(
        inventory=inventory,
        initiator=initiator,
        target=target,
        artifacts_root=root,
        run_id=run_id,
        git_commit=git_commit,
        repo_root=repo_root,
    )


def _host_banner(host: HostContext) -> str:
    if host.spec.mode == "local":
        return f"{host.spec.name}(local)"
    return f"{host.spec.name}(ssh:{host.spec.ssh_host})"


def prepare_host(host: HostContext, hugepages: int, skip_build: bool, skip_runtime: bool) -> None:
    print(f"\n=== prepare: {_host_banner(host)} ===")
    host.run(["python3", "--version"], check=True, capture_output=True)
    host.run(["bash", "--version"], check=True, capture_output=True)
    host.run(["which", "make"], check=True, capture_output=True)
    host.run(["which", "gcc"], check=False, capture_output=True)
    host.run(["which", "clang"], check=False, capture_output=True)

    if not host.exists(host.spec.repo_path):
        raise FileNotFoundError(f"repo_path does not exist on host {host.spec.name}: {host.spec.repo_path}")

    if host.spec.use_sudo:
        host.sudo_run(["true"], check=True)

    if not skip_build:
        host.run_shell(
            "./configure --with-rdma --without-crypto --disable-tests --without-vfio-user && "
            "JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN || echo 1) && make -j${JOBS}",
            cwd=host.spec.repo_path,
            check=True,
            capture_output=True,
        )

    if not skip_runtime and host.spec.use_sudo:
        host.sudo_run(["modprobe", "nvme-tcp"], check=False)
        host.sudo_run(["sysctl", "-w", f"vm.nr_hugepages={hugepages}"], check=False)
        host.sudo_run(["pkill", "-f", "spdk_tgt.*--wait-for-rpc"], check=False)


def run_prepare(ctx: PlatformContext, args: argparse.Namespace) -> Dict[str, Any]:
    seen: Dict[str, HostContext] = {}
    seen[ctx.initiator.spec.name] = ctx.initiator
    seen[ctx.target.spec.name] = ctx.target

    for host in seen.values():
        prepare_host(host, hugepages=args.hugepages, skip_build=args.skip_build, skip_runtime=args.skip_runtime)

    return {
        "status": "ok",
        "hosts": sorted(seen.keys()),
        "skip_build": args.skip_build,
        "skip_runtime": args.skip_runtime,
        "hugepages": args.hugepages,
    }


def has_flag(argv: Sequence[str], name: str) -> bool:
    return any(token == name or token.startswith(f"{name}=") for token in argv)


def normalize_scenario_args(argv: Sequence[str]) -> List[str]:
    out = list(argv)
    if out and out[0] == "--":
        return out[1:]
    return out


def scenario_common_argv(ctx: PlatformContext, target_log: str) -> List[str]:
    inv = ctx.inventory
    init = ctx.initiator.spec
    tgt = ctx.target.spec
    paths = inv.paths
    argv = [
        "--rpc-script", resolve_bin(init.repo_path, paths.rpc_py),
        "--spdk-tgt", resolve_bin(init.repo_path, paths.spdk_tgt),
        "--spdk-nvme-passthru", resolve_bin(init.repo_path, paths.spdk_nvme_passthru),
        "--trtype", inv.nvmeof.trtype,
        "--traddr", inv.nvmeof.traddr,
        "--trsvcid", inv.nvmeof.trsvcid,
        "--nqn", inv.nvmeof.nqn,
        "--hostnqn", inv.nvmeof.hostnqn,
        "--target-log", target_log,
    ]
    if inv.nvmeof.src_addr:
        argv.extend(["--src-addr", inv.nvmeof.src_addr])
    if inv.nvmeof.src_svcid:
        argv.extend(["--src-svcid", inv.nvmeof.src_svcid])
    if inv.nvmeof.pcie_bdf:
        argv.extend(["--pcie-bdf", inv.nvmeof.pcie_bdf])
    if inv.nvmeof.dataset_bdev:
        argv.extend(["--dataset-bdev", inv.nvmeof.dataset_bdev])
    if inv.nvmeof.backing_bdev:
        argv.extend(["--backing-bdev", inv.nvmeof.backing_bdev])

    if ctx.split_mode and tgt.mode == "ssh":
        argv.extend(["--target-ssh-host", tgt.ssh_host or ""])
        if tgt.ssh_user:
            argv.extend(["--target-ssh-user", tgt.ssh_user])
        argv.extend(["--target-ssh-port", str(tgt.ssh_port)])
        for opt in tgt.ssh_options:
            argv.extend(["--target-ssh-option", opt])
        if tgt.use_sudo:
            argv.append("--target-use-sudo")
        argv.extend(["--target-repo-path", tgt.repo_path])

    return argv


def apply_inventory_runtime_overrides(
    ctx: PlatformContext,
    argv: List[str],
    *,
    vector_mode: bool,
) -> List[str]:
    runtime = ctx.inventory.runtime

    mask = runtime.target_core_mask
    if mask:
        if vector_mode:
            if not has_flag(argv, "--target-core-mask"):
                argv.extend(["--target-core-mask", mask])
        else:
            if not has_flag(argv, "--core-mask"):
                argv.extend(["--core-mask", mask])

    compute_mask = runtime.target_compute_core_mask
    if compute_mask:
        if vector_mode:
            if not has_flag(argv, "--target-compute-core-mask"):
                argv.extend(["--target-compute-core-mask", compute_mask])
        else:
            if not has_flag(argv, "--compute-core-mask"):
                argv.extend(["--compute-core-mask", compute_mask])

    if runtime.initiator_passthru_lcores and not has_flag(argv, "--passthru-lcores"):
        argv.extend(["--passthru-lcores", runtime.initiator_passthru_lcores])

    if vector_mode and runtime.vector_prefill_workers is not None and not has_flag(argv, "--prefill-workers"):
        argv.extend(["--prefill-workers", str(runtime.vector_prefill_workers)])

    return argv
