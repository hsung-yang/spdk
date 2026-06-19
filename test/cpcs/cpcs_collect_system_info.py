#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

import argparse
import json
import os
import platform
import re
import shlex
import shutil
import socket
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def run_command(cmd: List[str]) -> Tuple[int, str, str]:
    try:
        cp = subprocess.run(cmd, text=True, capture_output=True, check=False)
        return cp.returncode, cp.stdout.strip(), cp.stderr.strip()
    except OSError as exc:
        return 127, "", str(exc)


def has_cmd(name: str) -> bool:
    return shutil.which(name) is not None


def read_text(path: str) -> Optional[str]:
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return None


def parse_kv_lines(text: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        out[k.strip()] = v.strip()
    return out


def parse_os_release() -> Dict[str, str]:
    content = read_text("/etc/os-release")
    if not content:
        return {}
    out: Dict[str, str] = {}
    for line in content.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k] = v.strip().strip('"')
    return out


def kb_to_gib_str(kb_value: int) -> str:
    return f"{(kb_value / 1024.0 / 1024.0):.2f} GiB"


def bytes_to_human(value: Optional[int]) -> str:
    if value is None or value < 0:
        return "n/a"
    units = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"]
    size = float(value)
    for unit in units:
        if size < 1024.0 or unit == units[-1]:
            return f"{size:.2f} {unit}"
        size /= 1024.0
    return f"{value} B"


def safe_int(value: Any) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def collect_host_info() -> Dict[str, Any]:
    os_release = parse_os_release()
    hostname = socket.gethostname()
    fqdn = socket.getfqdn()
    kernel_cmdline = read_text("/proc/cmdline")
    uptime_text = read_text("/proc/uptime")

    uptime_seconds: Optional[float] = None
    if uptime_text:
        first = uptime_text.split()[0]
        try:
            uptime_seconds = float(first)
        except ValueError:
            pass

    return {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "hostname": hostname,
        "fqdn": fqdn,
        "os_pretty_name": os_release.get("PRETTY_NAME"),
        "os_name": os_release.get("NAME"),
        "os_version": os_release.get("VERSION"),
        "kernel_release": platform.release(),
        "kernel_version": platform.version(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "uptime_seconds": uptime_seconds,
        "kernel_cmdline": kernel_cmdline,
    }


def collect_cpu_info(notes: List[str]) -> Dict[str, Any]:
    info: Dict[str, Any] = {}
    if has_cmd("lscpu"):
        rc, out, err = run_command(["lscpu"])
        if rc == 0 and out:
            kv = parse_kv_lines(out)
            info = {
                "source": "lscpu",
                "architecture": kv.get("Architecture"),
                "vendor_id": kv.get("Vendor ID"),
                "model_name": kv.get("Model name"),
                "cpu_count": kv.get("CPU(s)"),
                "sockets": kv.get("Socket(s)"),
                "cores_per_socket": kv.get("Core(s) per socket"),
                "threads_per_core": kv.get("Thread(s) per core"),
                "numa_nodes": kv.get("NUMA node(s)"),
                "min_mhz": kv.get("CPU min MHz"),
                "max_mhz": kv.get("CPU max MHz"),
                "l1d_cache": kv.get("L1d cache"),
                "l1i_cache": kv.get("L1i cache"),
                "l2_cache": kv.get("L2 cache"),
                "l3_cache": kv.get("L3 cache"),
            }
            return info
        notes.append(f"lscpu failed: rc={rc}, stderr={err or 'n/a'}")

    cpuinfo = read_text("/proc/cpuinfo")
    if cpuinfo:
        model = None
        vendor = None
        cpu_count = 0
        for line in cpuinfo.splitlines():
            if line.startswith("processor"):
                cpu_count += 1
            elif line.startswith("model name") and model is None and ":" in line:
                model = line.split(":", 1)[1].strip()
            elif line.startswith("vendor_id") and vendor is None and ":" in line:
                vendor = line.split(":", 1)[1].strip()
        return {
            "source": "/proc/cpuinfo",
            "architecture": platform.machine(),
            "vendor_id": vendor,
            "model_name": model,
            "cpu_count": str(cpu_count) if cpu_count else None,
        }

    notes.append("CPU info collection fallback failed: /proc/cpuinfo is unavailable.")
    return {"source": "unavailable"}


def parse_meminfo() -> Dict[str, int]:
    meminfo = read_text("/proc/meminfo")
    if not meminfo:
        return {}
    out: Dict[str, int] = {}
    for line in meminfo.splitlines():
        match = re.match(r"^([A-Za-z0-9_()]+):\s+([0-9]+)\s+kB$", line)
        if not match:
            continue
        out[match.group(1)] = int(match.group(2))
    return out


def collect_dimm_info(notes: List[str], skip_dmidecode: bool) -> Dict[str, Any]:
    if skip_dmidecode:
        return {"status": "skipped", "devices": []}

    if not has_cmd("dmidecode"):
        notes.append("dmidecode is not installed; DIMM inventory not collected.")
        return {"status": "missing_dmidecode", "devices": []}

    rc, out, err = run_command(["dmidecode", "-t", "memory"])
    if rc != 0:
        msg = err if err else "unknown error"
        notes.append(
            f"dmidecode -t memory failed (rc={rc}). Run as root to collect per-DIMM details. stderr={msg}"
        )
        return {"status": "dmidecode_failed", "error": msg, "devices": []}

    devices: List[Dict[str, str]] = []
    in_device = False
    current: Dict[str, str] = {}

    def flush_current() -> None:
        nonlocal current
        if not current:
            return
        size = current.get("Size", "")
        if size and size not in {"No Module Installed", "Not Installed", "Unknown"}:
            devices.append(
                {
                    "locator": current.get("Locator", ""),
                    "bank_locator": current.get("Bank Locator", ""),
                    "size": size,
                    "type": current.get("Type", ""),
                    "speed": current.get("Speed", ""),
                    "configured_speed": current.get("Configured Memory Speed", ""),
                    "manufacturer": current.get("Manufacturer", ""),
                    "part_number": current.get("Part Number", ""),
                    "serial_number": current.get("Serial Number", ""),
                }
            )
        current = {}

    for raw_line in out.splitlines():
        line = raw_line.strip()
        if line.startswith("Handle "):
            if in_device:
                flush_current()
            in_device = False
            continue
        if line == "Memory Device":
            if in_device:
                flush_current()
            in_device = True
            current = {}
            continue
        if not in_device:
            continue
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        current[k.strip()] = v.strip()

    if in_device:
        flush_current()

    return {"status": "ok", "devices": devices}


def collect_memory_info(notes: List[str], skip_dmidecode: bool) -> Dict[str, Any]:
    meminfo = parse_meminfo()
    if not meminfo:
        notes.append("/proc/meminfo is unavailable; memory totals could not be collected.")
    mem_total_kb = meminfo.get("MemTotal")
    mem_free_kb = meminfo.get("MemFree")
    mem_avail_kb = meminfo.get("MemAvailable")
    huge_total = meminfo.get("HugePages_Total")
    huge_free = meminfo.get("HugePages_Free")
    huge_size_kb = meminfo.get("Hugepagesize")

    return {
        "source": "/proc/meminfo" if meminfo else "unavailable",
        "mem_total_kb": mem_total_kb,
        "mem_total_human": kb_to_gib_str(mem_total_kb) if mem_total_kb else None,
        "mem_free_kb": mem_free_kb,
        "mem_available_kb": mem_avail_kb,
        "hugepages_total": huge_total,
        "hugepages_free": huge_free,
        "hugepage_size_kb": huge_size_kb,
        "dimm_inventory": collect_dimm_info(notes, skip_dmidecode),
    }


def collect_lsblk_info(notes: List[str]) -> Dict[str, Any]:
    if not has_cmd("lsblk"):
        notes.append("lsblk is not installed; disk inventory not collected.")
        return {"status": "missing_lsblk", "devices": []}

    rc, out, err = run_command(
        [
            "lsblk",
            "-J",
            "-d",
            "-o",
            "NAME,TYPE,SIZE,MODEL,SERIAL,ROTA,TRAN,VENDOR,REV",
        ]
    )
    if rc != 0:
        notes.append(f"lsblk failed: rc={rc}, stderr={err or 'n/a'}")
        return {"status": "lsblk_failed", "error": err, "devices": []}

    try:
        payload = json.loads(out)
    except json.JSONDecodeError as exc:
        notes.append(f"lsblk JSON parse failed: {exc}")
        return {"status": "lsblk_parse_failed", "error": str(exc), "devices": []}

    devices: List[Dict[str, str]] = []
    for dev in payload.get("blockdevices", []):
        dev_type = str(dev.get("type") or "")
        if dev_type not in {"disk", "nvme"}:
            continue
        rota = str(dev.get("rota") or "").strip()
        media = "SSD" if rota == "0" else "HDD" if rota == "1" else "unknown"
        devices.append(
            {
                "name": str(dev.get("name") or ""),
                "type": dev_type,
                "size": str(dev.get("size") or ""),
                "media": media,
                "transport": str(dev.get("tran") or ""),
                "model": str(dev.get("model") or "").strip(),
                "serial": str(dev.get("serial") or "").strip(),
                "vendor": str(dev.get("vendor") or "").strip(),
                "revision": str(dev.get("rev") or "").strip(),
            }
        )
    return {"status": "ok", "devices": devices}


def collect_nvme_info(notes: List[str]) -> Dict[str, Any]:
    if not has_cmd("nvme"):
        notes.append("nvme-cli is not installed; NVMe controller details not collected.")
        return {"status": "missing_nvme", "devices": []}

    rc, out, err = run_command(["nvme", "list", "-o", "json"])
    if rc != 0:
        notes.append(f"nvme list failed: rc={rc}, stderr={err or 'n/a'}")
        return {"status": "nvme_list_failed", "error": err, "devices": []}

    try:
        payload = json.loads(out)
    except json.JSONDecodeError as exc:
        notes.append(f"nvme list JSON parse failed: {exc}")
        return {"status": "nvme_parse_failed", "error": str(exc), "devices": []}

    raw_devices = payload.get("Devices") or payload.get("devices") or []
    devices: List[Dict[str, str]] = []
    for dev in raw_devices:
        size_bytes = safe_int(dev.get("PhysicalSize"))
        if size_bytes is None:
            size_bytes = safe_int(dev.get("UsedBytes"))
        devices.append(
            {
                "device_path": str(dev.get("DevicePath") or ""),
                "model_number": str(dev.get("ModelNumber") or dev.get("Model") or "").strip(),
                "serial_number": str(dev.get("SerialNumber") or dev.get("Serial") or "").strip(),
                "firmware": str(dev.get("Firmware") or dev.get("FWRev") or "").strip(),
                "size_bytes": size_bytes,
                "size_human": bytes_to_human(size_bytes),
            }
        )
    return {"status": "ok", "devices": devices}


def collect_storage_pci(notes: List[str]) -> Dict[str, Any]:
    if not has_cmd("lspci"):
        notes.append("lspci is not installed; storage PCIe controller list not collected.")
        return {"status": "missing_lspci", "controllers": []}

    rc, out, err = run_command(["lspci", "-Dnn"])
    if rc != 0:
        notes.append(f"lspci failed: rc={rc}, stderr={err or 'n/a'}")
        return {"status": "lspci_failed", "error": err, "controllers": []}

    keywords = (
        "non-volatile memory controller",
        "mass storage controller",
        "raid bus controller",
        "sata controller",
    )
    controllers = []
    for line in out.splitlines():
        line_lower = line.lower()
        if any(k in line_lower for k in keywords):
            controllers.append(line.strip())

    return {"status": "ok", "controllers": controllers}


def md_escape(value: Any) -> str:
    if value is None:
        return "n/a"
    text = str(value).strip()
    if not text:
        return "n/a"
    return text.replace("|", "\\|").replace("\n", "<br>")


def render_kv_table(rows: List[Tuple[str, Any]]) -> str:
    lines = [
        "| Item | Value |",
        "| --- | --- |",
    ]
    for key, value in rows:
        lines.append(f"| {md_escape(key)} | {md_escape(value)} |")
    return "\n".join(lines)


def render_dict_table(rows: List[Dict[str, Any]], columns: List[Tuple[str, str]]) -> str:
    headers = [label for _, label in columns]
    lines = [
        f"| {' | '.join(headers)} |",
        f"| {' | '.join(['---'] * len(columns))} |",
    ]
    for row in rows:
        cells = [md_escape(row.get(key)) for key, _ in columns]
        lines.append(f"| {' | '.join(cells)} |")
    return "\n".join(lines)


def format_uptime(seconds: Optional[float]) -> str:
    if seconds is None:
        return "n/a"
    total = int(seconds)
    days, rem = divmod(total, 86400)
    hours, rem = divmod(rem, 3600)
    minutes, sec = divmod(rem, 60)
    return f"{days}d {hours}h {minutes}m {sec}s"


def render_markdown(report: Dict[str, Any], title: str = "Experiment Setup") -> str:
    host = report["host"]
    cpu = report["cpu"]
    memory = report["memory"]
    storage = report["storage"]
    notes = report["notes"]

    lines: List[str] = [f"# {title}", ""]

    lines.append("## Host and OS")
    lines.append(
        render_kv_table(
            [
                ("Generated (UTC)", host.get("generated_at_utc")),
                ("Hostname", host.get("hostname")),
                ("FQDN", host.get("fqdn")),
                ("OS", host.get("os_pretty_name") or host.get("os_name")),
                ("Kernel", host.get("kernel_release")),
                ("Machine", host.get("machine")),
                ("Uptime", format_uptime(host.get("uptime_seconds"))),
            ]
        )
    )
    lines.append("")

    lines.append("## CPU")
    lines.append(
        render_kv_table(
            [
                ("Source", cpu.get("source")),
                ("Model", cpu.get("model_name")),
                ("Vendor", cpu.get("vendor_id")),
                ("Architecture", cpu.get("architecture")),
                ("Logical CPUs", cpu.get("cpu_count")),
                ("Sockets", cpu.get("sockets")),
                ("Cores per Socket", cpu.get("cores_per_socket")),
                ("Threads per Core", cpu.get("threads_per_core")),
                ("NUMA Nodes", cpu.get("numa_nodes")),
                ("CPU Min MHz", cpu.get("min_mhz")),
                ("CPU Max MHz", cpu.get("max_mhz")),
                ("L1d Cache", cpu.get("l1d_cache")),
                ("L1i Cache", cpu.get("l1i_cache")),
                ("L2 Cache", cpu.get("l2_cache")),
                ("L3 Cache", cpu.get("l3_cache")),
            ]
        )
    )
    lines.append("")

    lines.append("## Memory (DRAM)")
    lines.append(
        render_kv_table(
            [
                ("Source", memory.get("source")),
                ("MemTotal", memory.get("mem_total_human")),
                ("MemAvailable", kb_to_gib_str(memory["mem_available_kb"]) if memory.get("mem_available_kb") else "n/a"),
                ("MemFree", kb_to_gib_str(memory["mem_free_kb"]) if memory.get("mem_free_kb") else "n/a"),
                ("HugePages_Total", memory.get("hugepages_total")),
                ("HugePages_Free", memory.get("hugepages_free")),
                ("HugePage Size", f"{memory.get('hugepage_size_kb')} kB" if memory.get("hugepage_size_kb") else "n/a"),
            ]
        )
    )
    lines.append("")

    dimm_inventory = memory.get("dimm_inventory", {})
    dimms = dimm_inventory.get("devices", [])
    lines.append("### DIMM Inventory (dmidecode)")
    if dimms:
        lines.append(
            render_dict_table(
                dimms,
                [
                    ("locator", "Locator"),
                    ("size", "Size"),
                    ("type", "Type"),
                    ("speed", "Speed"),
                    ("configured_speed", "Configured Speed"),
                    ("manufacturer", "Manufacturer"),
                    ("part_number", "Part Number"),
                    ("serial_number", "Serial Number"),
                ],
            )
        )
    else:
        status = dimm_inventory.get("status", "unavailable")
        lines.append(f"- No DIMM details collected (status: `{md_escape(status)}`).")
    lines.append("")

    lines.append("## Block Devices (SSD/HDD)")
    block_devices = storage["block_devices"].get("devices", [])
    if block_devices:
        lines.append(
            render_dict_table(
                block_devices,
                [
                    ("name", "Name"),
                    ("type", "Type"),
                    ("size", "Size"),
                    ("media", "Media"),
                    ("transport", "Transport"),
                    ("model", "Model"),
                    ("vendor", "Vendor"),
                    ("serial", "Serial"),
                    ("revision", "FW/Rev"),
                ],
            )
        )
    else:
        status = storage["block_devices"].get("status", "unavailable")
        lines.append(f"- No block-device details collected (status: `{md_escape(status)}`).")
    lines.append("")

    lines.append("## NVMe Devices (nvme-cli)")
    nvme_devices = storage["nvme"].get("devices", [])
    if nvme_devices:
        lines.append(
            render_dict_table(
                nvme_devices,
                [
                    ("device_path", "Device"),
                    ("model_number", "Model"),
                    ("serial_number", "Serial"),
                    ("firmware", "Firmware"),
                    ("size_human", "Capacity"),
                ],
            )
        )
    else:
        status = storage["nvme"].get("status", "unavailable")
        lines.append(f"- No NVMe details collected (status: `{md_escape(status)}`).")
    lines.append("")

    lines.append("## Storage Controllers (PCIe)")
    controllers = storage["storage_pci"].get("controllers", [])
    if controllers:
        for ctrl in controllers:
            lines.append(f"- `{md_escape(ctrl)}`")
    else:
        status = storage["storage_pci"].get("status", "unavailable")
        lines.append(f"- No storage PCIe controller details collected (status: `{md_escape(status)}`).")
    lines.append("")

    if notes:
        lines.append("## Collection Notes")
        for note in notes:
            lines.append(f"- {md_escape(note)}")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Collect host system information for experiment setup sections "
            "(CPU, DRAM, SSD/NVMe, OS, and storage PCIe controllers)."
        )
    )
    parser.add_argument(
        "--output-markdown",
        "-o",
        default="-",
        help="Markdown output path. Use '-' for stdout (default: -).",
    )
    parser.add_argument(
        "--output-json",
        default=None,
        help="Optional JSON output path for archival/reproducibility.",
    )
    parser.add_argument(
        "--skip-dmidecode",
        action="store_true",
        help="Skip dmidecode even if installed.",
    )
    parser.add_argument(
        "--title",
        default="Experiment Setup",
        help="Top-level Markdown title.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    notes: List[str] = []

    report: Dict[str, Any] = {
        "host": collect_host_info(),
        "cpu": collect_cpu_info(notes),
        "memory": collect_memory_info(notes, args.skip_dmidecode),
        "storage": {
            "block_devices": collect_lsblk_info(notes),
            "nvme": collect_nvme_info(notes),
            "storage_pci": collect_storage_pci(notes),
        },
        "notes": notes,
        "collector": {
            "script": Path(__file__).name,
            "argv": [shlex.quote(a) for a in sys.argv],
            "euid": os.geteuid() if hasattr(os, "geteuid") else None,
        },
    }

    md = render_markdown(report, title=args.title)
    if args.output_markdown == "-":
        print(md, end="")
    else:
        Path(args.output_markdown).write_text(md, encoding="utf-8")

    if args.output_json:
        Path(args.output_json).write_text(json.dumps(report, indent=2), encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
