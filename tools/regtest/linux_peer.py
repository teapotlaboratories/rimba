"""Linux HaLow peer orchestration over ssh, for T2 tests that need a Linux node on the air.

Reach nodes by hostname only (chronium/chronite/chronosalt/chronogen) — never a raw IP
(.ai/AGENTS.md). `~/.ssh/config` resolves them.

STATUS: hardware-verified 2026-07-16. `bring_up_mesh(chronite)` brings the node up as a
`rimba-smesh` mesh point over ssh, and an ESP (`test-mesh-linux`) peers with it + pings it
(the `mesh-linux` T2 test). `ssh_run` / `radio_silence` are exercised + safe.
"""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Callable, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
#: The reference SAE mesh config a node needs at _MESH_CONF_PATH (pushed if missing).
_REF_CONF = REPO_ROOT / "docs" / "reference" / "captures" / "wpa-smesh.conf"

# Prepended to every remote command: the locale + PATH gotchas that bite every session
# (iw/morse_cli/wpa_supplicant_s1g live in /usr/sbin, off the non-login PATH).
_ENV = "export LC_ALL=C; export PATH=$PATH:/usr/sbin:/usr/local/bin;"


def ssh_run(host: str, cmd: str, timeout: int = 30, sudo_ok: bool = True) -> subprocess.CompletedProcess:
    """Run a command on a Linux node over ssh. Returns the CompletedProcess.

    Uses BatchMode so a missing key fails fast instead of hanging on a password prompt.
    """
    full = f"{_ENV} {cmd}"
    return subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=6", host, full],
        capture_output=True, text=True, timeout=timeout,
    )


def reachable(host: str) -> bool:
    try:
        cp = ssh_run(host, "echo ok", timeout=10)
        return cp.returncode == 0 and "ok" in cp.stdout
    except Exception:
        return False


def radio_silence(host: str) -> tuple[bool, str]:
    """Take a node's HaLow radio off the air: leave any mesh, kill the supplicant, wlan1 down.

    This is the Linux half of the standing radio-silent rule, and the teardown for every
    Linux peer this module brings up. Safe to call even if nothing was running.
    """
    cp = ssh_run(host,
                 "sudo iw dev wlan1 mesh leave 2>/dev/null; "
                 "sudo pkill -9 wpa_supplicant 2>/dev/null; "
                 "sudo ip link set wlan1 down 2>/dev/null; echo SILENCED",
                 timeout=25)
    ok = "SILENCED" in cp.stdout
    return ok, (cp.stdout.strip() or cp.stderr.strip())


#: The SAE mesh config the bench's `rimba-smesh` uses (docs/reference/rimba-bench-devices.md).
#: The node must already carry this at the given path, or bring_up_mesh pushes it.
_MESH_CONF_PATH = "/tmp/wpa-smesh.conf"


def _node(host: str):
    """The manifest LinuxNode registry entry for a host, or None (single source of truth)."""
    from . import manifest
    return manifest.LINUX_NODES.get(host)


def live_wlan1_mac(host: str) -> Optional[str]:
    """Read the node's actual wlan1 MAC over ssh (lower-case), or None."""
    cp = ssh_run(host, "sudo iw dev wlan1 info 2>/dev/null | awk '/addr/{print $2; exit}'")
    mac = cp.stdout.strip().lower()
    return mac if len(mac) == 17 and mac.count(":") == 5 else None


def live_wlan1_ipv4(host: str) -> Optional[str]:
    """Read the node's current wlan1 IPv4 over ssh (the mesh IP once the mesh is up), or None."""
    cp = ssh_run(host, "ip -4 -o addr show dev wlan1 2>/dev/null | "
                       "awk '{print $4}' | cut -d/ -f1 | head -1")
    ip = cp.stdout.strip()
    parts = ip.split(".")
    return ip if len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts) else None


def mesh_mac_matches(host: str) -> tuple[bool, str]:
    """Cross-check the node's LIVE wlan1 MAC against the manifest registry (which the ESP interop
    firmware is compiled against). Returns (ok, detail). A mismatch means the firmware's hardcoded
    peer MAC is stale for this node -- the ESP would never see it as the expected peer."""
    node = _node(host)
    if node is None or not node.mesh_mac:
        return True, f"{host}: no registry MAC to cross-check"
    live = live_wlan1_mac(host)
    if live is None:
        return True, f"{host}: could not read live wlan1 MAC"
    if live != node.mesh_mac.lower():
        return False, (f"{host}: live wlan1 MAC {live} != registry/firmware MAC {node.mesh_mac} "
                       f"-- the ESP interop app's hardcoded LINUX_MAC is stale for this node")
    return True, f"{host}: MAC {live} matches the registry"


def _push_config(host: str) -> bool:
    """scp the reference mesh config to the node's /tmp (it is tmpfs, wiped by a reboot)."""
    if not _REF_CONF.exists():
        return False
    try:
        cp = subprocess.run(
            ["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
             str(_REF_CONF), f"{host}:{_MESH_CONF_PATH}"],
            capture_output=True, text=True, timeout=30)
        return cp.returncode == 0
    except Exception:
        return False


def bring_up_mesh(host: str) -> tuple[bool, str, Optional[Callable[[], None]]]:
    """Bring a Linux node up as a member of the `rimba-smesh` SAE+AMPE mesh.

    Returns (ok, detail, teardown). teardown() takes the node back off the air.

    Hardware-verified 2026-07-16 with chronite. Follows the documented recipe: the mesh is
    started by wpa_supplicant_s1g with the S1G SAE config, NOT `iw mesh join` (which never
    starts the firmware mesh BSS). The config is pushed if missing (/tmp is tmpfs).
    """
    if not reachable(host):
        return False, f"{host} not reachable over ssh", None

    node = _node(host)
    if node is None:
        return False, f"{host}: not in the LINUX_NODES registry (manifest.py)", None
    mesh_ip = node.mesh_ip

    # The mesh JOIN is runtime-only + /tmp is tmpfs, so the config can vanish (e.g. a reboot).
    # Ensure it is present -- push the reference config if it is not already there.
    chk = ssh_run(host, f"test -f {_MESH_CONF_PATH} && echo HAVE || echo MISSING")
    if "HAVE" not in chk.stdout and not _push_config(host):
        return False, (f"{host}: {_MESH_CONF_PATH} absent and could not scp the reference "
                       f"config ({_REF_CONF})"), None

    # Start wpa_supplicant in mesh mode, then POLL for `type mesh` (the join is async and can
    # take longer than a fixed sleep -- polling makes the bring-up robust vs a timing flake).
    ssh_run(host,
            "sudo iw dev wlan1 mesh leave 2>/dev/null; sudo pkill -9 wpa_supplicant 2>/dev/null; "
            "sleep 1; sudo rm -f /var/run/wpa_supplicant_s1g/wlan1; "
            "sudo ip link set wlan1 down; sudo iw dev wlan1 set type managed; "
            "sudo ip link set wlan1 up; "
            f"sudo wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c {_MESH_CONF_PATH} "
            "-f /tmp/wpa-smesh.log -dd >/dev/null 2>&1; echo STARTED",
            timeout=30)
    poll = ssh_run(host,
                   "for i in $(seq 1 12); do "
                   "sudo iw dev wlan1 info 2>/dev/null | grep -q 'type mesh' && { echo MESHUP; break; }; "
                   "sleep 1; done; "
                   f"sudo ip addr add {mesh_ip}/24 dev wlan1 2>/dev/null; true",
                   timeout=25)
    if "MESHUP" not in poll.stdout:
        log = ssh_run(host, "sudo tail -3 /tmp/wpa-smesh.log 2>/dev/null | tr '\\n' '|'")
        return False, (f"{host}: mesh did not reach 'type mesh' within ~12s. "
                       f"wpa log tail: {log.stdout.strip()[-200:] or '(empty)'}"), None

    # Cross-check the live MAC vs the registry/firmware -- a mismatch means the ESP interop app's
    # hardcoded peer MAC is stale, so the ESP would never recognise this node as the expected peer.
    match_ok, match_detail = mesh_mac_matches(host)
    warn = "" if match_ok else f"  !! WARNING: {match_detail}"
    detail = f"{host}: rimba-smesh mesh point up, {mesh_ip}{warn}"

    def teardown():
        radio_silence(host)

    return True, detail, teardown


# --- AP (hostapd_s1g) bring-up, for the tp power tier -----------------------

#: The reference hostapd_s1g AP config (SSID rimba-ping / SAE rimbahalow / dtim1 / PMF / ch27) —
#: the same SSID/security the ESP SoftAP and the DUT (test-power) use, so one firmware serves
#: both AP paths. Pushed to the node's /tmp if missing (tmpfs, wiped by a reboot).
_AP_REF_CONF = REPO_ROOT / "docs" / "reference" / "captures" / "hostapd-rimba.conf"
_AP_CONF_PATH = "/tmp/hostapd-rimba.conf"


def _push_ap_config(host: str) -> bool:
    if not _AP_REF_CONF.exists():
        return False
    try:
        cp = subprocess.run(
            ["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
             str(_AP_REF_CONF), f"{host}:{_AP_CONF_PATH}"],
            capture_output=True, text=True, timeout=30)
        return cp.returncode == 0
    except Exception:
        return False


def bring_up_ap(host: str) -> tuple[bool, str, Optional[Callable[[], None]]]:
    """Bring a Linux node up as the `rimba-ping` hostapd_s1g SoftAP for the tp power tier.

    The authoritative AP the STA power-save ladder associates to (SSID rimba-ping, SAE,
    dtim_period=1, PMF, S1G ch27). The DUT (test-power) associates to it exactly as it does
    to the ESP SoftAP. Returns (ok, detail, teardown). Config pushed to /tmp if missing.
    """
    if not reachable(host):
        return False, f"{host} not reachable over ssh", None

    chk = ssh_run(host, f"test -f {_AP_CONF_PATH} && echo HAVE || echo MISSING")
    if "HAVE" not in chk.stdout and not _push_ap_config(host):
        return False, (f"{host}: {_AP_CONF_PATH} absent and could not scp the reference "
                       f"config ({_AP_REF_CONF})"), None

    # hostapd owns wlan1's mode (it sets type AP); clear any prior mesh/AP/supplicant first.
    ssh_run(host,
            "sudo iw dev wlan1 mesh leave 2>/dev/null; "
            "sudo pkill -9 wpa_supplicant 2>/dev/null; sudo pkill -9 hostapd_s1g 2>/dev/null; "
            "sleep 1; sudo ip link set wlan1 down 2>/dev/null; "
            f"sudo hostapd_s1g -B -P /tmp/hostapd-rimba.pid -f /tmp/hostapd-rimba.log {_AP_CONF_PATH} "
            ">/dev/null 2>&1; echo STARTED",
            timeout=30)
    poll = ssh_run(host,
                   "for i in $(seq 1 12); do "
                   "sudo iw dev wlan1 info 2>/dev/null | grep -q 'type AP' && { echo APUP; break; }; "
                   "sleep 1; done",
                   timeout=25)
    if "APUP" not in poll.stdout:
        log = ssh_run(host, "sudo tail -4 /tmp/hostapd-rimba.log 2>/dev/null | tr '\\n' '|'")
        return False, (f"{host}: hostapd_s1g did not reach 'type AP' within ~12s. "
                       f"log tail: {log.stdout.strip()[-220:] or '(empty)'}"), None

    detail = f"{host}: hostapd_s1g SoftAP 'rimba-ping' up (SAE, dtim1, PMF, ch27)"

    def teardown():
        ssh_run(host,
                "sudo pkill -9 hostapd_s1g 2>/dev/null; "
                "sudo ip link set wlan1 down 2>/dev/null; echo SILENCED", timeout=25)

    return True, detail, teardown
