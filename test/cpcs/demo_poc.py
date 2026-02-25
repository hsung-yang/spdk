#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2025 Samsung Electronics.
# All rights reserved.

import argparse
import json
import os
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
SPDK_ROOT = SCRIPT_DIR.parent.parent


def env_default(name, default):
    return os.environ.get(name, default)


RPC = env_default("RPC", str(SPDK_ROOT / "scripts" / "rpc.py"))
SPDK_TGT = env_default("SPDK_TGT", str(SPDK_ROOT / "build" / "bin" / "spdk_tgt"))
SPDK_NVME_PASSTHRU = env_default(
    "SPDK_NVME_PASSTHRU",
    str(SPDK_ROOT / "build" / "bin" / "spdk_nvme_passthru"),
)
SPDK_NVME_PASSTHRU_LCORES = env_default("SPDK_NVME_PASSTHRU_LCORES", "1")
RPC_SOCK = env_default("RPC_SOCK", "/var/tmp/spdk.sock")
NVME = env_default("NVME", "nvme")

NQN = env_default("NQN", "nqn.2024-01.io.spdk:cpcs-demo")
SERIAL = env_default("SERIAL", "CPCSDEMO001")
TRTYPE = env_default("TRTYPE", "TCP")
TRADDR = env_default("TRADDR", "127.0.0.1")
TRSVCID = env_default("TRSVCID", "4420")
NVME_TRTYPE = env_default("NVME_TRTYPE", TRTYPE.lower())
MAX_NAMESPACES = int(env_default("MAX_NAMESPACES", "1024"))

SLM_BDEV = env_default("SLM_BDEV", "SLM0")
SLM_NSID = int(env_default("SLM_NSID", "100"))
SLM_MB = int(env_default("SLM_MB", "64"))
SLM_GRANULARITY = int(env_default("SLM_GRANULARITY", "512"))
SLM_BACKEND = env_default("SLM_BACKEND", "slm")
VSLM_BASE_BDEV = env_default("VSLM_BASE_BDEV", "SLMBase0")
VSLM_SRAM_MB = int(env_default("VSLM_SRAM_MB", "8"))
CTRL_DEV = env_default("CTRL_DEV", "/dev/nvme0")
SLM_DEV = env_default("SLM_DEV", f"/dev/nvme0n{SLM_NSID}")

NVM_BDEV = env_default("NVM_BDEV", "NVM0")
NVM_NSID = int(env_default("NVM_NSID", "1"))
NVM_MB = int(env_default("NVM_MB", "64"))
NVM_DEV = env_default("NVM_DEV", f"/dev/nvme0n{NVM_NSID}")

CPCS_NSID = int(env_default("CPCS_NSID", "200"))
MAX_ACTIVATED = int(env_default("MAX_ACTIVATED", "16"))
MAX_MRS = int(env_default("MAX_MRS", "64"))
MAX_RANGES_PER_MRS = int(env_default("MAX_RANGES_PER_MRS", "8"))
LOAD_PROGRAM_GRAN = int(env_default("LOAD_PROGRAM_GRAN", "3"))

EBPF_PIND = int(env_default("EBPF_PIND", "10"))
EBPF_PTYPE = int(env_default("EBPF_PTYPE", "0xC0"), 0)
EBPF_PIT = int(env_default("EBPF_PIT", "0x01"), 0)
EBPF_PUID = int(env_default("EBPF_PUID", "0xEBF00001"), 0)
EBPF_SRC = SPDK_ROOT / "test" / "cpcs" / "ebpf" / "ebpf_mul64.c"

STATE_DIR = Path(env_default("STATE_DIR", "/tmp/cpcs_demo"))
PID_FILE = STATE_DIR / "spdk_tgt.pid"
LOG_FILE = STATE_DIR / "spdk_tgt.log"
DATA_DIR = STATE_DIR / "data"
INPUT_BIN = DATA_DIR / "input.bin"
MRS_BIN = DATA_DIR / "mrs.bin"
SUM64_BIN = DATA_DIR / "sum64.bin"
MAX64_BIN = DATA_DIR / "max64.bin"
MIN64_BIN = DATA_DIR / "min64.bin"
MEMCOPY_DESC_BIN = DATA_DIR / "memcpy_desc.bin"
MEMCOPY_OUT_BIN = DATA_DIR / "memcpy_out.bin"
NVM_COPY_DESC_BIN = DATA_DIR / "nvm_copy_desc.bin"
NVM_COPY_OUT_BIN = DATA_DIR / "nvm_copy_out.bin"
NVM_PAR_COPY_SRC_BIN = DATA_DIR / "nvm_par_copy_src.bin"
NVM_PAR_COPY_DESC_BIN = DATA_DIR / "nvm_par_copy_desc.bin"
NVM_PAR_COPY_OUT_BIN = DATA_DIR / "nvm_par_copy_out.bin"
NVM_PAR_COPY_CHUNK_BIN = DATA_DIR / "nvm_par_copy_chunk.bin"
EBPF_OBJ = DATA_DIR / "ebpf_mul64.o"
EBPF_BIN = DATA_DIR / "ebpf_mul64.bin"
EBPF_OUT = DATA_DIR / "ebpf_out.bin"

SLM_PAR_COPY_RANGE_COUNT = int(env_default("SLM_PAR_COPY_RANGE_COUNT", "129"))
SLM_PAR_COPY_START_LBA = int(env_default("SLM_PAR_COPY_START_LBA", "128"))
SLM_PAR_COPY_DEST_OFFSET = int(env_default("SLM_PAR_COPY_DEST_OFFSET", "4096"))
SLM_PAR_COPY_READ_CHUNK = int(env_default("SLM_PAR_COPY_READ_CHUNK", "4096"))

SLM_COPY_DESC_FMT_2H = 0x2
SLM_COPY_DESC_FMT_3H = 0x3
SLM_COPY_DESC_FMT_4H = 0x4
SLM_COPY_DESC_FMT_SHIFT = 8

STEP_NO = 0
COLOR_RESET = "\033[0m"
COLOR_TITLE = "\033[1;36m"
COLOR_DESC = "\033[2m"
COLOR_CMD = "\033[33m"
COLOR_LOG = "\033[90m"
COLOR_RESULT = "\033[32m"


def use_color(stream):
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("TERM", "dumb") == "dumb":
        return False
    return stream.isatty()


def color(text, code, stream=sys.stdout):
    if not use_color(stream):
        return text
    return f"{code}{text}{COLOR_RESET}"


def step(msg, desc=None):
    global STEP_NO
    STEP_NO += 1
    print("")
    print(color(f"== Step {STEP_NO}: {msg} ==", COLOR_TITLE))
    if desc:
        print(color(desc, COLOR_DESC))


def pause(auto):
    if auto:
        return
    input("Press Enter to continue...")


def quote_cmd(cmd):
    return " ".join(shlex.quote(str(c)) for c in cmd)


def run(cmd, check=True):
    print(color(f"+ {quote_cmd(cmd)}", COLOR_CMD))
    return subprocess.run(cmd, check=check)


def run_capture(cmd):
    print(color(f"+ {quote_cmd(cmd)}", COLOR_CMD, sys.stderr), file=sys.stderr)
    result = subprocess.run(cmd, text=True, capture_output=True)
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, cmd, output=output)
    return output


def is_executable(path):
    return os.path.isfile(path) and os.access(path, os.X_OK)


def ensure_hugepages(target=1024):
    try:
        with open("/proc/sys/vm/nr_hugepages", "r", encoding="utf-8") as f:
            current = int(f.read().strip() or "0")
    except (OSError, ValueError):
        return

    if current >= target:
        return

    setup_script = SPDK_ROOT / "scripts" / "setup.sh"
    if not setup_script.exists():
        print("Hugepages low and setup.sh not found; continuing anyway.", file=sys.stderr)
        return

    print(f"Hugepages low ({current}), running setup.sh to allocate hugepages...", file=sys.stderr)
    run([str(setup_script)])


def find_tool(candidates):
    for name in candidates:
        path = shutil.which(name)
        if path:
            return path
    return ""


def rpc_cmd():
    if is_executable(RPC):
        return [RPC]
    return ["python3", RPC]


def rpc(*args):
    run(rpc_cmd() + ["-s", RPC_SOCK] + list(args))


def rpc_quiet(*args):
    subprocess.run(
        rpc_cmd() + ["-s", RPC_SOCK] + list(args),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def rpc_capture(*args):
    result = subprocess.run(
        rpc_cmd() + ["-s", RPC_SOCK] + list(args),
        text=True,
        capture_output=True,
        check=True,
    )
    return result.stdout


def transport_exists(trtype):
    try:
        output = rpc_capture("nvmf_get_transports")
        transports = json.loads(output)
    except (subprocess.SubprocessError, json.JSONDecodeError):
        return False
    for item in transports:
        if str(item.get("trtype", "")).lower() == trtype.lower():
            return True
    return False


def subsystem_exists(nqn):
    try:
        output = rpc_capture("nvmf_get_subsystems", nqn)
        subsystems = json.loads(output)
    except (subprocess.SubprocessError, json.JSONDecodeError):
        return False
    return len(subsystems) > 0


def bdev_exists(name):
    try:
        output = rpc_capture("bdev_get_bdevs", "-b", name)
        bdevs = json.loads(output)
    except (subprocess.SubprocessError, json.JSONDecodeError):
        return False
    return len(bdevs) > 0


def wait_for_rpc():
    for _ in range(50):
        try:
            subprocess.run(
                rpc_cmd() + ["-s", RPC_SOCK, "bdev_get_bdevs"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
            )
            return
        except subprocess.CalledProcessError:
            time.sleep(0.2)
    print(f"Timed out waiting for RPC socket: {RPC_SOCK}", file=sys.stderr)
    sys.exit(1)


def start_target():
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    if PID_FILE.exists():
        try:
            pid = int(PID_FILE.read_text().strip())
            os.kill(pid, 0)
            print(f"SPDK target already running (pid {pid}).")
            return
        except Exception:
            pass

    with LOG_FILE.open("w") as log:
        proc = subprocess.Popen([SPDK_TGT, "-m", "0x1"], stdout=log, stderr=log)
    PID_FILE.write_text(str(proc.pid))
    wait_for_rpc()


def stop_target():
    if not PID_FILE.exists():
        return
    try:
        pid = int(PID_FILE.read_text().strip())
        os.kill(pid, 0)
        run(["kill", str(pid)], check=False)
    except Exception:
        pass
    PID_FILE.unlink(missing_ok=True)


def cleanup_rpc(slm_backend):
    has_subsystem = subsystem_exists(NQN)

    if has_subsystem:
        rpc_quiet("cpcs_ns_delete", "--subsystem-nqn", NQN, "--nsid", str(CPCS_NSID))
    if bdev_exists(SLM_BDEV):
        if slm_backend == "vslm":
            rpc_quiet("bdev_vslm_delete", "--name", SLM_BDEV)
        else:
            rpc_quiet("bdev_slm_delete", "--name", SLM_BDEV)
    if slm_backend == "vslm" and bdev_exists(VSLM_BASE_BDEV):
        rpc_quiet("bdev_malloc_delete", VSLM_BASE_BDEV)
    if bdev_exists(NVM_BDEV):
        rpc_quiet("bdev_malloc_delete", NVM_BDEV)
    if has_subsystem:
        rpc_quiet("nvmf_delete_subsystem", NQN)


def disconnect_nvme():
    try:
        out = subprocess.run([NVME, "list-subsys"], text=True, capture_output=True, check=False)
        if NQN in (out.stdout or ""):
            run([NVME, "disconnect", "-n", NQN], check=False)
    except FileNotFoundError:
        return


def is_block_device(path):
    try:
        return stat.S_ISBLK(os.stat(path).st_mode)
    except Exception:
        return False


def detect_ctrl_name(output):
    found = False
    for line in output.splitlines():
        if f"NQN={NQN}" in line:
            found = True
            continue
        if found and line.strip().startswith("+-"):
            parts = line.strip().split()
            if len(parts) >= 2:
                return parts[1]
    return ""


def parse_result_hex(output):
    match = re.search(r"result[:=]\\s*0x([0-9a-fA-F]+)", output)
    if not match:
        return ""
    return match.group(1)


def build_ebpf_program():
    if not EBPF_SRC.exists():
        print(f"eBPF source not found at {EBPF_SRC}", file=sys.stderr)
        sys.exit(1)

    DATA_DIR.mkdir(parents=True, exist_ok=True)

    clang = find_tool(["clang", "clang-14", "clang-15", "clang-16"])
    objcopy = find_tool(["llvm-objcopy", "llvm-objcopy-14", "llvm-objcopy-15", "objcopy"])

    if not clang or not objcopy:
        print("Missing clang/llvm-objcopy. Install llvm + clang to build eBPF program.",
              file=sys.stderr)
        sys.exit(1)

    run([clang, "-O2", "-target", "bpf", "-c", str(EBPF_SRC), "-o", str(EBPF_OBJ)])
    run([objcopy, "-O", "binary", "--only-section=cpcs", str(EBPF_OBJ), str(EBPF_BIN)])
    if EBPF_BIN.stat().st_size == 0:
        run([objcopy, "-O", "binary", "--only-section=.text", str(EBPF_OBJ), str(EBPF_BIN)])
    if EBPF_BIN.stat().st_size == 0:
        print("eBPF bytecode section is empty. Check compiler/section names.", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Run CPCS PoC demo inside the VM.")
    parser.add_argument("--auto", action="store_true", help="Skip prompts")
    parser.add_argument(
        "--slm-backend",
        choices=["slm", "vslm"],
        default=SLM_BACKEND,
        help="Choose SLM backend: slm (memory) or vslm (tiered memory).",
    )
    args = parser.parse_args()
    slm_backend = args.slm_backend

    if os.geteuid() != 0:
        print("Please run as root (sudo) inside the VM.", file=sys.stderr)
        return 1

    if not is_executable(SPDK_TGT):
        print(f"spdk_tgt not found at {SPDK_TGT}. Build SPDK first.", file=sys.stderr)
        return 1
    if not is_executable(SPDK_NVME_PASSTHRU):
        print(f"spdk_nvme_passthru not found at {SPDK_NVME_PASSTHRU}. Build SPDK first.", file=sys.stderr)
        return 1
    if not (is_executable(NVME) or shutil.which(NVME)):
        print("nvme CLI not found. Install nvme-cli or set NVME=/path/to/nvme.", file=sys.stderr)
        return 1

    ensure_hugepages()

    step(
        "Start SPDK target",
        "Launch spdk_tgt and wait for the RPC socket to become available.",
    )
    start_target()
    pause(args.auto)

    step(
        "Ensure clean demo state",
        "Remove any leftover CPCS/SLM objects from previous demo runs.",
    )
    cleanup_rpc(slm_backend)
    pause(args.auto)

    step(
        "Create NVMf transport + subsystem",
        "Ensure a TCP transport exists and create the CPCS demo subsystem.",
    )
    if transport_exists(TRTYPE):
        print(f"Transport {TRTYPE} already exists; skipping create.")
    else:
        rpc("nvmf_create_transport", "-t", TRTYPE)
    rpc("nvmf_create_subsystem", NQN, "-s", SERIAL, "-m", str(MAX_NAMESPACES), "-a")
    pause(args.auto)

    step(
        "Create NVM and SLM bdevs and add as namespaces",
        "Create an LBA-backed bdev (NVM) and a memory-backed bdev (SLM), then attach both as namespaces.",
    )
    rpc("bdev_malloc_create", "-b", NVM_BDEV, str(NVM_MB), "512")
    rpc("nvmf_subsystem_add_ns", NQN, NVM_BDEV, "-n", str(NVM_NSID))
    if slm_backend == "vslm":
        rpc("bdev_malloc_create", "-b", VSLM_BASE_BDEV, str(SLM_MB), "512")
        rpc("bdev_vslm_create", "--name", SLM_BDEV, "--base-bdev-name", VSLM_BASE_BDEV,
            "--sram-size-mb", str(VSLM_SRAM_MB), "--nsid", str(SLM_NSID))
    else:
        rpc("bdev_slm_create", "--name", SLM_BDEV, "--nsid", str(SLM_NSID),
            "--size-mb", str(SLM_MB), "--granularity", str(SLM_GRANULARITY))
    rpc("nvmf_subsystem_add_ns", NQN, SLM_BDEV, "-n", str(SLM_NSID))
    rpc("nvmf_subsystem_add_listener", NQN, "-t", TRTYPE, "-a", TRADDR, "-s", TRSVCID)
    rpc("bdev_get_bdevs", "-b", SLM_BDEV)
    pause(args.auto)

    step(
        "Create CPCS compute namespace",
        "Create the compute namespace that will run CPCS programs.\n"
        f"Note: LOAD_PROGRAM_GRAN=2^{LOAD_PROGRAM_GRAN} bytes to allow small eBPF binaries.",
    )
    rpc("cpcs_ns_create", "--subsystem-nqn", NQN, "--nsid", str(CPCS_NSID),
        "--max-activated", str(MAX_ACTIVATED), "--max-mrs", str(MAX_MRS),
        "--max-ranges-per-mrs", str(MAX_RANGES_PER_MRS),
        "--load-program-gran", str(LOAD_PROGRAM_GRAN))
    pause(args.auto)

    step(
        "Install builtins and list programs",
        "Register built-in programs (e.g., sum64) and list them.",
    )
    rpc("cpcs_program_install_builtins", "--subsystem-nqn", NQN, "--nsid", str(CPCS_NSID))
    rpc("cpcs_program_list", "--subsystem-nqn", NQN, "--nsid", str(CPCS_NSID))
    pause(args.auto)

    step(
        "Connect host and discover devices",
        "Connect via nvme-cli and show controller/namespace metadata.",
    )
    if NVME_TRTYPE == "tcp":
        subprocess.run(["modprobe", "nvme-tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run([NVME, "disconnect", "-n", NQN], check=False)
    run([NVME, "connect", "-t", NVME_TRTYPE, "-n", NQN, "-a", TRADDR, "-s", TRSVCID])
    run([NVME, "list"])
    list_subsys = subprocess.run([NVME, "list-subsys"], text=True, capture_output=True, check=False)
    print(list_subsys.stdout or "")

    ctrl_name = detect_ctrl_name(list_subsys.stdout or "")
    ctrl_dev = CTRL_DEV
    if ctrl_name:
        ctrl_dev = f"/dev/{ctrl_name}"

    slm_dev = SLM_DEV
    nvm_dev = NVM_DEV
    if not is_block_device(slm_dev):
        try:
            list_json = subprocess.run([NVME, "list", "-o", "json"],
                                       text=True, capture_output=True, check=False)
            devs = json.loads(list_json.stdout or "{}").get("Devices", [])
            for dev in devs:
                if dev.get("SerialNumber") == SERIAL and dev.get("Namespace") == SLM_NSID:
                    slm_dev = dev.get("DevicePath", slm_dev)
                    break
            for dev in devs:
                if dev.get("SerialNumber") == SERIAL and dev.get("Namespace") == NVM_NSID:
                    nvm_dev = dev.get("DevicePath", nvm_dev)
                    break
        except Exception:
            pass

    if not is_block_device(slm_dev) and ctrl_name:
        slm_dev = f"/dev/{ctrl_name}n{SLM_NSID}"
    if not is_block_device(slm_dev) and ctrl_name:
        fallback = f"/dev/{ctrl_name}n1"
        if is_block_device(fallback):
            slm_dev = fallback

    if not is_block_device(nvm_dev) and ctrl_name:
        nvm_dev = f"/dev/{ctrl_name}n{NVM_NSID}"
    if not is_block_device(nvm_dev) and ctrl_name:
        fallback = f"/dev/{ctrl_name}n1"
        if is_block_device(fallback):
            nvm_dev = fallback

    if not os.path.exists(ctrl_dev) and is_block_device(slm_dev):
        ctrl_from_slm = slm_dev.rsplit("n", 1)[0]
        if os.path.exists(ctrl_from_slm):
            ctrl_dev = ctrl_from_slm
        else:
            ctrl_dev = slm_dev

    for _ in range(20):
        if os.path.exists(ctrl_dev):
            break
        time.sleep(0.2)

    if not os.path.exists(ctrl_dev):
        print(f"CTRL_DEV not found at {ctrl_dev}. Set CTRL_DEV and re-run.", file=sys.stderr)
        return 1
    if not is_block_device(nvm_dev):
        print(f"NVM namespace device not found. Set NVM_DEV env var.", file=sys.stderr)
        return 1

    run([NVME, "id-ctrl", ctrl_dev])
    run([NVME, "list-ns", ctrl_dev], check=False)
    run([NVME, "id-ns", ctrl_dev, "-n", str(NVM_NSID)], check=False)
    run([NVME, "id-ns", ctrl_dev, "-n", str(SLM_NSID)], check=False)
    run([NVME, "id-ns", ctrl_dev, "-n", str(CPCS_NSID)], check=False)
    pause(args.auto)

    step(
        "Write demo data into NVM namespace (NVM Write opcode 0x01)",
        "Write 64 little-endian uint64 values [1..64] to the NVM namespace using LBA addressing.\n"
        "Fields:\n"
        "  opcode=0x01        NVM Write\n"
        "  nsid=1             NVM namespace\n"
        "  start-block=0      SLBA\n"
        "  data-size=512      payload length\n"
        "  input-file=input.bin 64x uint64 LE values",
    )
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    with INPUT_BIN.open("wb") as f:
        for v in range(1, 65):
            f.write(struct.pack("<Q", v))
    run([NVME, "write", nvm_dev, "--data", str(INPUT_BIN),
         "--data-size", "512", "--start-block", "0"])
    pause(args.auto)

    step(
        "Copy NVM data into SLM namespace (SLM Memory Copy opcode 0x01, desc fmt 2h)",
        "Copy 512 bytes from NVM namespace LBA 0 into SLM destination byte address 0.\n"
        "Fields:\n"
        "  opcode=0x01        SLM Memory Copy\n"
        "  nsid=100           SLM namespace (destination)\n"
        "  cdw2=512           Length (bytes)\n"
        "  cdw10=0            Destination byte address (low)\n"
        "  cdw11=0            Destination byte address (high)\n"
        f"  cdw12=0x{(SLM_COPY_DESC_FMT_2H << SLM_COPY_DESC_FMT_SHIFT):04x}       desc_fmt={SLM_COPY_DESC_FMT_2H} (bits 11:8), nr=0 (one descriptor)\n"
        "  data-len=32        one 32-byte copy descriptor\n"
        "  input-file=nvm_copy_desc.bin layout: snsid, slba, nlb",
    )
    with NVM_COPY_DESC_BIN.open("wb") as f:
        f.write(struct.pack("<IIQHHIIHH", NVM_NSID, 0, 0, 0, 0, 0, 0, 0, 0))
    run([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x01", "--nsid", str(SLM_NSID),
        "--cdw2", "512", "--cdw3", "0",
        "--cdw10", "0", "--cdw11", "0", "--cdw12",
        f"0x{(SLM_COPY_DESC_FMT_2H << SLM_COPY_DESC_FMT_SHIFT):04x}",
        "--data-len", "32", "--write", "--input-file", str(NVM_COPY_DESC_BIN),
    ])
    run([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x02", "--nsid", str(SLM_NSID),
        "--cdw10", "0", "--cdw11", "0", "--cdw12", "512",
        "--data-len", "512", "--read", "--output-file", str(NVM_COPY_OUT_BIN),
    ])
    if NVM_COPY_OUT_BIN.read_bytes() != INPUT_BIN.read_bytes():
        print("NVM to SLM Memory Copy verification failed", file=sys.stderr)
        return 1
    print(color("NVM to SLM Memory Copy verified", COLOR_RESULT))
    pause(args.auto)

    if SLM_PAR_COPY_RANGE_COUNT <= 0 or SLM_PAR_COPY_RANGE_COUNT > 256:
        print("SLM_PAR_COPY_RANGE_COUNT must be in [1, 256]", file=sys.stderr)
        return 1
    if SLM_PAR_COPY_START_LBA < 0:
        print("SLM_PAR_COPY_START_LBA must be >= 0", file=sys.stderr)
        return 1
    if SLM_PAR_COPY_DEST_OFFSET < 0:
        print("SLM_PAR_COPY_DEST_OFFSET must be >= 0", file=sys.stderr)
        return 1
    if SLM_PAR_COPY_READ_CHUNK <= 0:
        print("SLM_PAR_COPY_READ_CHUNK must be > 0", file=sys.stderr)
        return 1

    step(
        "Copy NVM data into SLM with many desc fmt 2h descriptors (parallel read path)",
        "Issue a multi-range SLM Memory Copy request to exercise in-target parallel read submission.\n"
        "Fields:\n"
        "  opcode=0x01        SLM Memory Copy\n"
        "  nsid=100           SLM namespace (destination)\n"
        f"  cdw2={SLM_PAR_COPY_RANGE_COUNT * 512}        Length (bytes)\n"
        f"  cdw10={SLM_PAR_COPY_DEST_OFFSET}      Destination byte address (low)\n"
        "  cdw11=0            Destination byte address (high)\n"
        f"  cdw12=0x{((SLM_COPY_DESC_FMT_2H << SLM_COPY_DESC_FMT_SHIFT) | (SLM_PAR_COPY_RANGE_COUNT - 1)):04x}       desc_fmt={SLM_COPY_DESC_FMT_2H}, nr={SLM_PAR_COPY_RANGE_COUNT - 1}\n"
        f"  data-len={SLM_PAR_COPY_RANGE_COUNT * 32}       {SLM_PAR_COPY_RANGE_COUNT} descriptors (32 bytes each)\n"
        f"  read-chunk={SLM_PAR_COPY_READ_CHUNK}      chunk size for verification reads\n"
        "  input-file=nvm_par_copy_desc.bin layout: snsid, slba, nlb",
    )
    par_total_len = SLM_PAR_COPY_RANGE_COUNT * 512
    par_desc_len = SLM_PAR_COPY_RANGE_COUNT * 32
    par_cdw12 = (SLM_COPY_DESC_FMT_2H << SLM_COPY_DESC_FMT_SHIFT) | (SLM_PAR_COPY_RANGE_COUNT - 1)
    par_pattern = bytearray()
    for i in range(SLM_PAR_COPY_RANGE_COUNT):
        par_pattern.extend(struct.pack("<Q", 0x1111111100000000 + i) * (512 // 8))
    NVM_PAR_COPY_SRC_BIN.write_bytes(par_pattern)
    run([NVME, "write", nvm_dev, "--data", str(NVM_PAR_COPY_SRC_BIN),
         "--data-size", str(par_total_len), "--start-block", str(SLM_PAR_COPY_START_LBA),
         "--block-count", str(SLM_PAR_COPY_RANGE_COUNT - 1)])
    with NVM_PAR_COPY_DESC_BIN.open("wb") as f:
        for i in range(SLM_PAR_COPY_RANGE_COUNT):
            f.write(struct.pack("<IIQHHIIHH", NVM_NSID, 0, SLM_PAR_COPY_START_LBA + i, 0, 0, 0, 0, 0, 0))
    run([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x01", "--nsid", str(SLM_NSID),
        "--cdw2", str(par_total_len), "--cdw3", "0",
        "--cdw10", str(SLM_PAR_COPY_DEST_OFFSET), "--cdw11", "0", "--cdw12", hex(par_cdw12),
        "--data-len", str(par_desc_len), "--write", "--input-file", str(NVM_PAR_COPY_DESC_BIN),
    ])
    par_read_back = bytearray()
    read_offset = 0
    while read_offset < par_total_len:
        read_len = min(SLM_PAR_COPY_READ_CHUNK, par_total_len - read_offset)
        run([
            SPDK_NVME_PASSTHRU,
            "--lcores", SPDK_NVME_PASSTHRU_LCORES,
            "--no-rpc-server",
            "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
            "--opcode", "0x02", "--nsid", str(SLM_NSID),
            "--cdw10", str(SLM_PAR_COPY_DEST_OFFSET + read_offset), "--cdw11", "0", "--cdw12", str(read_len),
            "--data-len", str(read_len), "--read", "--output-file", str(NVM_PAR_COPY_CHUNK_BIN),
        ])
        par_read_back.extend(NVM_PAR_COPY_CHUNK_BIN.read_bytes())
        read_offset += read_len
    NVM_PAR_COPY_OUT_BIN.write_bytes(par_read_back)
    if bytes(par_read_back) != NVM_PAR_COPY_SRC_BIN.read_bytes():
        print("Parallel desc fmt 2h Memory Copy verification failed", file=sys.stderr)
        return 1
    print(color(
        f"Parallel desc fmt 2h Memory Copy verified ({SLM_PAR_COPY_RANGE_COUNT} descriptors)",
        COLOR_RESULT,
    ))
    pause(args.auto)

    step(
        "Copy SLM data within the namespace (SLM Memory Copy opcode 0x01, desc fmt 4h)",
        "Copy 512 bytes from offset 0 to offset 512 using SLM Memory Copy.\n"
        "Fields:\n"
        "  opcode=0x01        SLM Memory Copy\n"
        "  nsid=100           SLM namespace\n"
        "  cdw2=512           Length (bytes)\n"
        "  cdw10=512          Destination byte address (low)\n"
        "  cdw11=0            Destination byte address (high)\n"
        f"  cdw12=0x{(SLM_COPY_DESC_FMT_4H << SLM_COPY_DESC_FMT_SHIFT):04x}       desc_fmt={SLM_COPY_DESC_FMT_4H} (bits 11:8), nr=0 (one descriptor)\n"
        "  data-len=32        one 32-byte copy descriptor\n"
        "  input-file=memcpy_desc.bin layout: snsid, saddr, nbyte",
    )
    with MEMCOPY_DESC_BIN.open("wb") as f:
        f.write(struct.pack("<IIQIHHII", SLM_NSID, 0, 0, 512, 0, 0, 0, 0))
    run([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x01", "--nsid", str(SLM_NSID),
        "--cdw2", "512", "--cdw3", "0",
        "--cdw10", "512", "--cdw11", "0", "--cdw12",
        f"0x{(SLM_COPY_DESC_FMT_4H << SLM_COPY_DESC_FMT_SHIFT):04x}",
        "--data-len", "32", "--write", "--input-file", str(MEMCOPY_DESC_BIN),
    ])
    run([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x02", "--nsid", str(SLM_NSID),
        "--cdw10", "512", "--cdw11", "0", "--cdw12", "512",
        "--data-len", "512", "--read", "--output-file", str(MEMCOPY_OUT_BIN),
    ])
    if MEMCOPY_OUT_BIN.read_bytes() != INPUT_BIN.read_bytes():
        print("SLM Memory Copy verification failed", file=sys.stderr)
        return 1
    print(color("SLM Memory Copy verified", COLOR_RESULT))
    pause(args.auto)

    step(
        "Create Memory Range Set (admin opcode 0x89)",
        "Create a Memory Range Set that points to the SLM data.\n"
        "Fields:\n"
        "  opcode=0x89        CPCS MRS create/manage\n"
        "  namespace-id=200   CPCS namespace (compute)\n"
        "  cdw10=0            MACT=0 (create)\n"
        "  cdw11=1            NUMR=1 range descriptor\n"
        "  data-len=32        one 32-byte range descriptor\n"
        "  input-file=mrs.bin layout: MNID(4) LEN(4) SB(8) ATTR(16)\n"
        "  result cdw0        RSID (range set ID) returned by controller",
    )
    with MRS_BIN.open("wb") as f:
        f.write(struct.pack("<IIQ16s", SLM_NSID, 65536, 0, b"\\x00" * 16))
    mrs_out = run_capture([
        NVME, "admin-passthru", ctrl_dev,
        "--opcode=0x89", f"--namespace-id={CPCS_NSID}",
        "--cdw10=0", "--cdw11=1",
        "--data-len", "32", "--write", "--input-file", str(MRS_BIN),
    ])
    print(color(mrs_out, COLOR_LOG), end="")
    rsid_hex = parse_result_hex(mrs_out) or "1"
    rsid = int(rsid_hex, 16)
    print(color(f"MRS RSID={rsid}", COLOR_RESULT))
    pause(args.auto)

    step(
        "Execute builtin sum64 (I/O opcode 0x01)",
        "Execute sum64 over the MRS; expected result is 2080.\n"
        "Fields:\n"
        "  opcode=0x01        CPCS Execute\n"
        "  nsid=200           CPCS namespace (compute)\n"
        "  cdw2               (RSID << 16) | PIND, PIND=2 for sum64\n"
        "  cdw3=0             reserved\n"
        "  cdw4=24            execute descriptor length (bytes)\n"
        "  data-len=24        execute descriptor payload\n"
        "  input-file=sum64.bin layout: mr_id(8) off(8) len(8)",
    )
    with SUM64_BIN.open("wb") as f:
        f.write(struct.pack("<QQQ", 1, 0, 512))
    cdw2 = (rsid << 16) | 2
    exec_out = run_capture([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x01", "--nsid", str(CPCS_NSID),
        "--cdw2", str(cdw2), "--cdw3", "0", "--cdw4", "24",
        "--data-len", "24", "--write", "--input-file", str(SUM64_BIN),
    ])
    print(color(exec_out, COLOR_LOG), end="")
    exec_hex = parse_result_hex(exec_out)
    if exec_hex:
        exec_val = int(exec_hex, 16)
        print(color(f"Execute return value: {exec_val} (expected 2080)", COLOR_RESULT))
    pause(args.auto)

    step(
        "Execute builtin max64 (I/O opcode 0x01)",
        "Execute max64 over the MRS; expected result is 64.\n"
        "Fields:\n"
        "  opcode=0x01        CPCS Execute\n"
        "  nsid=200           CPCS namespace (compute)\n"
        "  cdw2               (RSID << 16) | PIND, PIND=3 for max64\n"
        "  cdw3=0             reserved\n"
        "  cdw4=24            execute descriptor length (bytes)\n"
        "  data-len=24        execute descriptor payload\n"
        "  input-file=max64.bin layout: mr_id(8) off(8) len(8)",
    )
    with MAX64_BIN.open("wb") as f:
        f.write(struct.pack("<QQQ", 1, 0, 512))
    cdw2 = (rsid << 16) | 3
    exec_out = run_capture([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x01", "--nsid", str(CPCS_NSID),
        "--cdw2", str(cdw2), "--cdw3", "0", "--cdw4", "24",
        "--data-len", "24", "--write", "--input-file", str(MAX64_BIN),
    ])
    print(color(exec_out, COLOR_LOG), end="")
    exec_hex = parse_result_hex(exec_out)
    if exec_hex:
        exec_val = int(exec_hex, 16)
        print(color(f"Execute return value: {exec_val} (expected 64)", COLOR_RESULT))
    pause(args.auto)

    step(
        "Execute builtin min64 (I/O opcode 0x01)",
        "Execute min64 over the MRS; expected result is 1.\n"
        "Fields:\n"
        "  opcode=0x01        CPCS Execute\n"
        "  nsid=200           CPCS namespace (compute)\n"
        "  cdw2               (RSID << 16) | PIND, PIND=4 for min64\n"
        "  cdw3=0             reserved\n"
        "  cdw4=24            execute descriptor length (bytes)\n"
        "  data-len=24        execute descriptor payload\n"
        "  input-file=min64.bin layout: mr_id(8) off(8) len(8)",
    )
    with MIN64_BIN.open("wb") as f:
        f.write(struct.pack("<QQQ", 1, 0, 512))
    cdw2 = (rsid << 16) | 4
    exec_out = run_capture([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x01", "--nsid", str(CPCS_NSID),
        "--cdw2", str(cdw2), "--cdw3", "0", "--cdw4", "24",
        "--data-len", "24", "--write", "--input-file", str(MIN64_BIN),
    ])
    print(color(exec_out, COLOR_LOG), end="")
    exec_hex = parse_result_hex(exec_out)
    if exec_hex:
        exec_val = int(exec_hex, 16)
        print(color(f"Execute return value: {exec_val} (expected 1)", COLOR_RESULT))
    pause(args.auto)

    step(
        "Build eBPF program (mul64)",
        "Compile eBPF bytecode for a user-defined program that multiplies two uint64 values.\n"
        "It reads 16 bytes from MRS #1 offset 0, writes the product to offset 16, and returns it.",
    )
    build_ebpf_program()
    pause(args.auto)

    step(
        "Load eBPF program (admin opcode 0x85)",
        "Download the eBPF program into CPCS.\n"
        "Fields:\n"
        "  opcode=0x85        CPCS Load Program\n"
        "  namespace-id=200   CPCS namespace (compute)\n"
        "  cdw10              PIND + PTYPE + SEL + PIT\n"
        f"    PIND={EBPF_PIND} PTYPE=0x{EBPF_PTYPE:x} PIT=0x{EBPF_PIT:x} SEL=0 (load)\n"
        "  cdw11              PSIZE (bytes)\n"
        "  cdw12/cdw13        PUID (program identifier)\n"
        "  cdw14              NUMB (bytes in this transfer)\n"
        "  cdw15              LOFF (load offset, 0 for first chunk)",
    )
    ebpf_size = EBPF_BIN.stat().st_size
    cdw10 = (EBPF_PIND & 0xFFFF) | ((EBPF_PTYPE & 0xFF) << 16) | ((0 & 0x1) << 24) | ((EBPF_PIT & 0x7) << 25)
    run([
        NVME, "admin-passthru", ctrl_dev,
        "--opcode=0x85", f"--namespace-id={CPCS_NSID}",
        f"--cdw10={cdw10}", f"--cdw11={ebpf_size}",
        f"--cdw12={EBPF_PUID & 0xFFFFFFFF}", f"--cdw13={(EBPF_PUID >> 32) & 0xFFFFFFFF}",
        f"--cdw14={ebpf_size}", "--cdw15=0",
        "--data-len", str(ebpf_size), "--write", "--input-file", str(EBPF_BIN),
    ])
    pause(args.auto)

    step(
        "Activate eBPF program (admin opcode 0x88)",
        "Activate the downloaded program so it can execute.\n"
        "Fields:\n"
        "  opcode=0x88        CPCS Program Activation\n"
        "  namespace-id=200   CPCS namespace (compute)\n"
        f"  cdw10              PIND={EBPF_PIND}, SEL=1 (activate)",
    )
    cdw10 = (EBPF_PIND & 0xFFFF) | (1 << 16)
    run([
        NVME, "admin-passthru", ctrl_dev,
        "--opcode=0x88", f"--namespace-id={CPCS_NSID}",
        f"--cdw10={cdw10}",
    ])
    pause(args.auto)

    step(
        "Execute eBPF mul64 (I/O opcode 0x01)",
        "Run the eBPF program over the MRS.\n"
        "It multiplies the first two uint64 values in SLM and returns the product.\n"
        "Fields:\n"
        "  opcode=0x01        CPCS Execute\n"
        "  nsid=200           CPCS namespace (compute)\n"
        f"  cdw2               (RSID << 16) | PIND, PIND={EBPF_PIND}\n"
        "  cdw3=0             reserved\n"
        "  cdw4=0             no execute descriptor payload",
    )
    cdw2 = (rsid << 16) | EBPF_PIND
    exec_out = run_capture([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x01", "--nsid", str(CPCS_NSID),
        "--cdw2", str(cdw2), "--cdw3", "0", "--cdw4", "0",
    ])
    print(color(exec_out, COLOR_LOG), end="")
    exec_hex = parse_result_hex(exec_out)
    if exec_hex:
        exec_val = int(exec_hex, 16)
        print(color(f"Execute return value: {exec_val} (expected 2)", COLOR_RESULT))
    pause(args.auto)

    step(
        "Validate eBPF output in SLM (SLM Memory Read opcode 0x02)",
        "Read back the product written by eBPF from SLM offset 16.\n"
        "Fields:\n"
        "  opcode=0x02        SLM Memory Read\n"
        "  nsid=100           SLM namespace\n"
        "  cdw10=16           Starting Byte (SB) low\n"
        "  cdw11=0            Starting Byte (SB) high\n"
        "  cdw12=8            Read Length (bytes)\n"
        "  output-file=ebpf_out.bin",
    )
    run([
        SPDK_NVME_PASSTHRU,
        "--lcores", SPDK_NVME_PASSTHRU_LCORES,
        "--no-rpc-server",
        "--io-cmd", "--trtype", TRTYPE, "--traddr", TRADDR, "--trsvcid", TRSVCID, "--subnqn", NQN,
        "--opcode", "0x02", "--nsid", str(SLM_NSID),
        "--cdw10", "16", "--cdw11", "0", "--cdw12", "8",
        "--data-len", "8", "--read", "--output-file", str(EBPF_OUT),
    ])
    prod = struct.unpack("<Q", EBPF_OUT.read_bytes())[0]
    print(color(f"SLM result at offset 16: {prod} (expected 2)", COLOR_RESULT))
    pause(args.auto)

    step(
        "Cleanup CPCS objects and stop target",
        "Disconnect and remove CPCS/SLM objects, then stop the target.",
    )
    disconnect_nvme()
    cleanup_rpc(slm_backend)
    stop_target()
    pause(args.auto)

    print("")
    print("Demo complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
