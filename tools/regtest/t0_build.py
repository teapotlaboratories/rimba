"""T0 — the build matrix.

Every app x board compiles via `make`, and the generated sdkconfig carries a real
country code and the board's chip target.

WHAT T0 PROVES: the tree still compiles against the current halow/morselib/ESP-IDF,
for every app, on every board. This is the tier that catches a stack bump removing or
renaming a morselib symbol — cheaply, with no hardware.

WHAT T0 DOES NOT PROVE: anything at runtime. A T0-green tree can still have a dead
radio, a broken mesh, or a silently wrong regulatory config on air.

Why the country-code assertion is part of T0 and not T1:
  morselib's umac_interface.c gates radio bring-up on the country code — with "??" it
  returns MMWLAN_CHANNEL_LIST_NOT_SET *before* it ever talks to the MM6108. The symptom
  is insidious: the BCF still parses, mmwlan_get_version returns UNAVAILABLE, the chip
  id prints as garbage and the MAC reads 00:00:00:00:00:00 — i.e. it looks exactly like
  dead hardware, and it cost a whole bench session on 2026-07-13 before the country code
  was spotted. It is a BUILD-TIME invariant (it comes from the board overlay, which a
  bare `idf.py build` bypasses), so it is caught here, in the tier that needs no
  hardware, rather than being rediscovered on the bench.
"""

from __future__ import annotations

import time
from typing import Optional

from . import manifest as M
from .common import (
    FAIL,
    FIRMWARE_DIR,
    PASS,
    SKIP,
    XFAIL,
    XPASS,
    Reporter,
    Result,
    make,
    read_sdkconfig_value,
    sdkconfig_path,
)


def _check_fw_blob(rep: Reporter) -> None:
    """Assert the pinned MM6108 firmware blob is EXACTLY the bench-standard version (1.17.8).

    A build-time, no-hardware tripwire. The whole bench (ESP + Linux nodes) runs one fw version;
    a silent bump to 1.17.9 roughly DOUBLES STA power-save current (the exact regression the tp
    tier exists to catch). Size + sha256 uniquely pin the version, so this catches the drift at the
    cheapest possible choke point -- before a single board is flashed.
    """
    import hashlib
    from .common import REPO_ROOT

    t0 = time.time()
    p = REPO_ROOT / M.FW_BLOB_PATH
    if not p.exists():
        rep.add(Result("T0", "fw-blob version-pin", FAIL,
                       detail=f"pinned fw blob missing: {M.FW_BLOB_PATH}",
                       duration_s=time.time() - t0))
        return
    data = p.read_bytes()
    size, sha = len(data), hashlib.sha256(data).hexdigest()
    ver = M.FW_SIZE_BY_VERSION.get(size, "unknown")
    ev = (f"{M.FW_BLOB_PATH}\n  size {size} B (expected {M.EXPECTED_FW_SIZE})\n"
          f"  sha256 {sha}\n  expected {M.EXPECTED_FW_SHA256}")
    if size == M.EXPECTED_FW_SIZE and sha == M.EXPECTED_FW_SHA256:
        rep.add(Result("T0", "fw-blob version-pin", PASS,
                       detail=f"mm6108.bin = {M.EXPECTED_FW_VERSION} "
                              f"({M.EXPECTED_FW_SIZE} B, sha {sha[:8]}…)",
                       duration_s=time.time() - t0, evidence=ev,
                       meta={"fw_version": M.EXPECTED_FW_VERSION, "size": size, "sha256": sha}))
        return
    if ver != "unknown" and ver != M.EXPECTED_FW_VERSION:
        why = (f"fw blob is {ver} (size {size}), NOT the pinned {M.EXPECTED_FW_VERSION} -- a silent "
               f"firmware bump. 1.17.9 roughly DOUBLES STA power-save current (the regression the tp "
               f"tier exists to catch). Re-pin vendor/morse-firmware to {M.EXPECTED_FW_VERSION}.")
    else:
        why = (f"fw blob does not match the pinned {M.EXPECTED_FW_VERSION}: size {size} "
               f"(expected {M.EXPECTED_FW_SIZE}), sha {sha[:12]}… -- unrecognised firmware "
               f"drift (a re-pin, a thin-LMAC swap, or a rebuild).")
    rep.add(Result("T0", "fw-blob version-pin", FAIL, detail=why,
                   duration_s=time.time() - t0, evidence=ev,
                   meta={"got_size": size, "got_sha256": sha, "got_version": ver}))


def _mirror_body(raw: bytes):
    """Strip a C source's leading /* */ banner, returning (body_lines, None) or (None, reason).

    A wrong file shape returns a reason (a caller FAIL), never a silent pass. Robust by construction:
    operates on BYTES (encoding-agnostic); collapses CRLF and lone CR to LF (line-ending-agnostic);
    anchors the body on the shared first `#include` SENTINEL, not a fixed line count (so re-editing
    either banner to a different length can't shift the boundary); asserts the region between the banner
    and that sentinel is blank so drift can't hide ABOVE the boundary; and drops one trailing empty line
    (trailing-newline-agnostic). Only line terminators and the final newline are normalized -- every
    other byte (indentation, interior whitespace) is preserved, so real content drift still fails.
    """
    norm = raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    lines = norm.split(b"\n")
    inc = next((i for i, ln in enumerate(lines) if ln.startswith(b"#include")), None)
    if inc is None:
        return None, "no `#include` sentinel found (empty or unexpected source shape)"
    header = b"\n".join(lines[:inc])
    if b"/*" not in header or b"*/" not in header:
        return None, "no leading /* */ banner before the first #include"
    if header.split(b"/*", 1)[0].strip():
        return None, "unexpected content before the leading /* banner"
    if header.split(b"*/", 1)[1].strip():
        return None, "non-blank content between the banner and the first #include (drift could hide here)"
    body = lines[inc:]
    if body and body[-1] == b"":
        body = body[:-1]
    return body, None


def _check_clone_mirrors(rep: Reporter) -> None:
    """Assert every faithful test-* clone's app_main.c BODY still matches its rimba-* source.

    A no-hardware source-drift tripwire (the verbatim analog of _check_fw_blob). A mesh-gate T2 fixture
    is a body clone of a shipped example; if someone patches one app_main.c and forgets the other, the
    fixture silently exercises stale code. The intentionally-different /* */ banner is excluded via the
    first-`#include` sentinel, and the banner-to-sentinel gap is asserted blank so drift can't hide there.
    A missing or mis-shaped file is a FAIL, never a silent pass. One Result per (clone, file); any FAIL
    flips rep.green and gates T0. The pairs live in M.CLONE_MIRRORS.
    """
    import difflib
    from .common import REPO_ROOT

    for clone, (prod, files) in sorted(M.CLONE_MIRRORS.items()):
        for rel in files:
            t0 = time.time()
            name = f"clone-mirror: {clone}/{rel}"
            targets = {
                "clone":  (clone, REPO_ROOT / "firmware" / clone / rel),
                "source": (prod,  REPO_ROOT / "firmware" / prod / rel),
            }
            bodies, aborted = {}, False
            for label, (app, path) in targets.items():
                try:
                    raw = path.read_bytes()
                except OSError as e:
                    rep.add(Result("T0", name, FAIL,
                                   detail=f"{label} file unreadable: firmware/{app}/{rel} "
                                          f"({type(e).__name__})",
                                   evidence=str(e), duration_s=time.time() - t0))
                    aborted = True
                    break
                body, err = _mirror_body(raw)
                if err is not None:
                    rep.add(Result("T0", name, FAIL,
                                   detail=f"{label} firmware/{app}/{rel}: {err}",
                                   duration_s=time.time() - t0))
                    aborted = True
                    break
                bodies[label] = body
            if aborted:
                continue

            cb, sb = bodies["clone"], bodies["source"]
            if cb == sb:
                rep.add(Result("T0", name, PASS,
                               detail=f"{clone}/{rel} body == {prod}/{rel} "
                                      f"({len(cb)} lines below the banner)",
                               duration_s=time.time() - t0,
                               meta={"clone": clone, "source": prod, "file": rel, "body_lines": len(cb)}))
                continue

            diff = difflib.unified_diff(
                [x.decode("utf-8", "replace") for x in cb],
                [x.decode("utf-8", "replace") for x in sb],
                fromfile=f"firmware/{clone}/{rel}", tofile=f"firmware/{prod}/{rel}",
                lineterm="", n=2)
            ev = "\n".join(list(diff)[:40]) or "(bodies differ in length only)"
            rep.add(Result("T0", name, FAIL,
                           detail=(f"{clone}/{rel} has DRIFTED from its source {prod}/{rel}: the "
                                   f"app_main.c body below the banner no longer matches. Re-sync them "
                                   f"(copy the body, keep each file's own /* */ banner): "
                                   f"diff firmware/{clone}/{rel} firmware/{prod}/{rel}"),
                           evidence=ev, duration_s=time.time() - t0,
                           meta={"clone": clone, "source": prod, "file": rel}))


def _tail(text: str, lines: int = 12) -> str:
    """The last N non-empty lines — enough to identify a compile error."""
    keep = [l for l in text.splitlines() if l.strip()]
    return "\n".join(keep[-lines:])


def _first_error(text: str) -> str:
    """Pull the first real compiler/cmake error out of a long build log."""
    for line in text.splitlines():
        low = line.lower()
        if "error:" in low or "fatal error" in low or "cmake error" in low:
            return line.strip()
    return ""


def run(apps: Optional[list[str]] = None, boards: Optional[list[str]] = None,
        quiet: bool = False) -> Reporter:
    rep = Reporter("T0", quiet=quiet)

    # Drift check first: an app in the tree but not the manifest is an untested hole.
    for err in M.check_manifest_covers_tree(FIRMWARE_DIR):
        rep.add(Result("T0", "manifest-covers-tree", FAIL, detail=err))

    # Version-pin the MM6108 firmware blob: a no-hardware tripwire for a silent fw bump (1.17.9
    # roughly doubles STA power-save current -- the regression the tp tier exists to catch).
    _check_fw_blob(rep)

    # Clone-drift tripwire: a faithful test-* fixture must not silently diverge from the rimba-* example
    # it clones (app_main.c body below the banner), or the fixture would test stale code. No hardware.
    _check_clone_mirrors(rep)

    selected = [a for a in M.T0_APPS if not apps or a.name in apps]
    if apps:
        unknown = set(apps) - {a.name for a in M.APPS}
        for u in sorted(unknown):
            rep.add(Result("T0", f"app:{u}", FAIL, detail="no such app in the manifest"))

    # Record the deliberate exclusions so they are visible in the baseline rather
    # than looking like coverage we forgot.
    for a in M.APPS:
        if not a.boards and (not apps or a.name in apps):
            rep.add(Result("T0", f"{a.name} (excluded)", SKIP,
                           detail=a.excluded_reason or "excluded from the matrix"))

    for app in selected:
        for board in app.boards:
            if boards and board not in boards:
                continue
            _build_one(rep, app, board)

    return rep


def _build_one(rep: Reporter, app: M.App, board: str) -> None:
    name = f"{app.name} x {board}"
    kb = M.known_broken(board)  # (reason, signature) or None
    t0 = time.time()
    try:
        cp = make("build", app.name, board, timeout=1800)
    except Exception as e:  # a timeout is a real failure, not a crash
        rep.add(Result("T0", name, FAIL, detail=f"make raised: {e}",
                       duration_s=time.time() - t0))
        return
    dur = time.time() - t0
    out = cp.stdout + cp.stderr

    if cp.returncode != 0:
        err = _first_error(out) or _tail(out, 8)
        if kb:
            reason, signature = kb
            if signature in out:
                # Expected: fails for exactly the documented reason. Report, don't gate.
                rep.add(Result(
                    "T0", name, XFAIL,
                    detail=f"known-broken board: {err}",
                    duration_s=dur,
                    evidence=f"make build APP={app.name} BOARD={board} -> exit {cp.returncode} "
                             f"(signature {signature!r} present)\nreason on record: {reason}",
                    meta={"known_broken": True},
                ))
                return
            # Fails, but NOT for the recorded reason -- a new, real break. Do NOT hide it.
            rep.add(Result(
                "T0", name, FAIL,
                detail=f"board is known-broken for a DIFFERENT reason -- expected signature "
                       f"{signature!r} absent, so this is a NEW failure:\n{err}",
                duration_s=dur,
                evidence=f"make build APP={app.name} BOARD={board}\n{_tail(out, 25)}",
            ))
            return
        rep.add(Result(
            "T0", name, FAIL,
            detail=f"make build exited {cp.returncode}\n{err}",
            duration_s=dur,
            evidence=f"make build APP={app.name} BOARD={board}\n{_tail(out, 25)}",
        ))
        return

    if kb:
        # It built. The recorded reason is stale (or someone fixed it) — say so loudly,
        # because a stale exclusion silently hides every future regression on this board.
        rep.add(Result(
            "T0", name, XPASS, duration_s=dur,
            detail=f"board '{board}' is marked KNOWN_BROKEN_BOARDS but this build SUCCEEDED. "
                   f"Remove the manifest entry.\nreason on record: {kb[0]}",
            meta={"known_broken": True},
        ))
        return

    # The build succeeded — now assert the config it was built WITH is sane.
    problems = []

    cc = read_sdkconfig_value(app.name, board, "CONFIG_HALOW_COUNTRY_CODE")
    if cc is None:
        # Only meaningful for apps that pull in the halow component at all.
        if app.radio:
            problems.append("CONFIG_HALOW_COUNTRY_CODE absent from the generated sdkconfig")
    elif cc == "??":
        problems.append(
            'CONFIG_HALOW_COUNTRY_CODE="??" -- morselib will refuse the radio '
            "(MMWLAN_CHANNEL_LIST_NOT_SET) and it will look like dead hardware. "
            "The board overlay was not applied."
        )
    elif not cc.strip():
        problems.append("CONFIG_HALOW_COUNTRY_CODE is empty")

    want_target = _board_target(board)
    got_target = read_sdkconfig_value(app.name, board, "CONFIG_IDF_TARGET")
    if want_target and got_target != want_target:
        problems.append(f"CONFIG_IDF_TARGET={got_target!r}, board overlay says {want_target!r}")

    if problems:
        rep.add(Result(
            "T0", name, FAIL,
            detail="build OK but the config is wrong:\n  " + "\n  ".join(problems),
            duration_s=dur,
            evidence=f"sdkconfig: {sdkconfig_path(app.name, board)}",
            meta={"country": cc, "target": got_target},
        ))
        return

    rep.add(Result(
        "T0", name, PASS, duration_s=dur,
        evidence=f"make build APP={app.name} BOARD={board} -> exit 0; "
                 f"CONFIG_HALOW_COUNTRY_CODE={cc!r} CONFIG_IDF_TARGET={got_target!r}",
        meta={"country": cc, "target": got_target},
    ))


_TARGET_CACHE: dict[str, Optional[str]] = {}


def _board_target(board: str) -> Optional[str]:
    """The chip target the board overlay declares (the board owns its target)."""
    if board in _TARGET_CACHE:
        return _TARGET_CACHE[board]
    import re
    from .common import BOARDS_DIR

    p = BOARDS_DIR / board / "sdkconfig.defaults"
    val = None
    if p.exists():
        m = re.search(r'^CONFIG_IDF_TARGET="([^"]*)"', p.read_text(), re.M)
        if m:
            val = m.group(1)
    _TARGET_CACHE[board] = val
    return val
