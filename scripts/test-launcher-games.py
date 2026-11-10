#!/usr/bin/env python3
"""Multi-game install and launch contract, through the real launcher and extractor.

    test-launcher-games.py LAUNCHER EXTRACTOR [--sly1-iso ISO] [--sly2-iso ISO]
                           [--sly3-iso ISO] [--extract]

Without disc images it checks disc identification, wrong-game rejection and
the Sly 2 / Sly 3 launch command lines with synthetic images and a fake runtime.
With --sly2-iso / --sly3-iso it also validates the real discs, and with
--extract installs them (copies about 2.9 / 3.6 GB) into a temporary portable
profile and verifies them.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 2048


def iso_image(boot, exe=b"\0" * 64, blocks=64):
    """A small ISO9660 image with SYSTEM.CNF and an executable in its root."""
    image = bytearray(32 * SECTOR)

    def both32(value):
        return struct.pack("<I", value) + struct.pack(">I", value)

    def record(sector, size, flags, name):
        length = 33 + len(name) + (0 if len(name) % 2 else 1)
        body = bytearray(length)
        body[0] = length
        body[2:10] = both32(sector)
        body[10:18] = both32(size)
        body[25] = flags
        body[32] = len(name)
        body[33:33 + len(name)] = name
        return bytes(body)

    cnf = f"BOOT2 = cdrom0:\\{boot};1\r\nVER = 1.00\r\nVMODE = NTSC\r\n".encode()
    pvd = bytearray(SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[80:88] = both32(blocks)
    pvd[156:156 + 34] = record(18, SECTOR, 2, b"\0")
    image[16 * SECTOR:17 * SECTOR] = pvd
    root = record(18, SECTOR, 2, b"\0") + record(18, SECTOR, 2, b"\1") + \
        record(20, len(cnf), 0, b"SYSTEM.CNF;1") + record(21, len(exe), 0, boot.encode() + b";1") + \
        record(22, 4, 0, b"IOPRP300.IMG;1")
    image[18 * SECTOR:18 * SECTOR + len(root)] = root
    image[20 * SECTOR:20 * SECTOR + len(cnf)] = cnf
    image[21 * SECTOR:21 * SECTOR + len(exe)] = exe
    image[22 * SECTOR:22 * SECTOR + 4] = b"IOP!"
    return bytes(image)


def run(command, **kwargs):
    return subprocess.run([str(c) for c in command], capture_output=True, timeout=kwargs.pop("timeout", 30), **kwargs)


def identify(extractor, iso):
    result = run([extractor, "--iso", iso, "--identify"])
    assert result.returncode == 0, result
    return json.loads(result.stdout.decode().strip().splitlines()[-1])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("launcher", type=pathlib.Path)
    p.add_argument("extractor", type=pathlib.Path)
    p.add_argument("--sly1-iso", type=pathlib.Path)
    p.add_argument("--sly2-iso", type=pathlib.Path)
    p.add_argument("--sly3-iso", type=pathlib.Path)
    p.add_argument("--extract", action="store_true")
    a = p.parse_args()
    launcher, extractor = a.launcher.resolve(), a.extractor.resolve()
    work = pathlib.Path(tempfile.mkdtemp(prefix="cane games ")).resolve()
    try:
        # ---- identification and wrong-game rejection (synthetic images)
        images = {}
        for boot in ("SCUS_971.98", "SCUS_973.16", "SCUS_974.64", "SCES_509.17", "SLUS_200.00"):
            images[boot] = work / f"{boot}.iso"
            images[boot].write_bytes(iso_image(boot))
        expected = {"SCUS_971.98": ("supported", "sly1"), "SCUS_973.16": ("supported", "sly2"),
                    "SCUS_974.64": ("supported", "sly3"), "SCES_509.17": ("supported", "sly1"),
                    "SLUS_200.00": ("unknown", None)}
        for boot, (kind, game) in expected.items():
            found = identify(extractor, images[boot])
            assert (found["kind"], found["game"], found["boot_file"]) == (kind, game, boot), (boot, found)
        out = work / "out"
        # A Sly 2 image whose executable is not the USA release.
        result = run([extractor, "--iso", images["SCUS_973.16"], "--output", out, "--game", "sly2", "--non-interactive"])
        assert result.returncode != 0 and b"Unsupported" in result.stderr and not out.exists(), result
        # Wrong game for the requested install.
        result = run([extractor, "--iso", images["SCUS_971.98"], "--output", out, "--game", "sly2", "--non-interactive"])
        assert result.returncode != 0 and b"not Sly 2: Band of Thieves" in result.stderr and not out.exists(), result
        # A Sly 3 image whose executable is not the USA release (routed to the Sly 3 installer by default).
        result = run([extractor, "--iso", images["SCUS_974.64"], "--output", out, "--non-interactive"])
        assert result.returncode != 0 and b"Unsupported" in result.stderr and b"SCUS-97464" in result.stderr \
            and not out.exists(), result
        # Sly 1 Europe is supported (the PCBRX merge): a European image whose executable is not the
        # real release's is refused as damaged, and the message lists Europe among the accepted discs.
        result = run([extractor, "--iso", images["SCES_509.17"], "--output", out, "--non-interactive"])
        assert result.returncode != 0 and b"Unsupported or damaged" in result.stderr \
            and b"Europe (SCES_509.17)" in result.stderr and not out.exists(), result
        result = run([extractor, "--iso", images["SCUS_973.16"], "--game", "sly3", "--non-interactive"])
        assert result.returncode != 0 and b"not Sly 3: Honor Among Thieves" in result.stderr, result
        result = run([extractor, "--iso", images["SCUS_973.16"], "--game", "sly4", "--non-interactive"])
        assert result.returncode != 0 and b"Unknown or unsupported game" in result.stderr, result

        # ---- Sly 2 launch contract (portable profile, fake runtime)
        portable = work / "portable copy"
        bin_dir = portable / "bin"
        bin_dir.mkdir(parents=True)
        copied = bin_dir / launcher.name
        shutil.copy2(launcher, copied)
        for name in ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll"):
            if (launcher.parent / name).is_file():
                shutil.copy2(launcher.parent / name, bin_dir / name)
        (portable / "portable.txt").write_text("ProjectCane portable mode\n")
        env = dict(os.environ)
        data = portable / "data"
        subprocess.run([str(copied), "--help"], env=env, check=True, capture_output=True, timeout=30)
        sly2 = data / "installations/sly2"
        sly2.mkdir(parents=True)
        # (asset version 3: the converted files with PC audio)
        (sly2 / "extraction_manifest.json").write_bytes(b"{}")
        (sly2 / "installation.json").write_text(json.dumps({
            "schema": 1, "game": "sly2", "asset_version": 3, "layout": "files",
            "disc_sha1": "de2af92a54de71e33edc8a160c31c0337a95e6f5",
            "files": [{"path": "extraction_manifest.json", "size": 2,
                       "sha256": hashlib.sha256(b"{}").hexdigest()}]}))
        result = run([copied, "--play", "sly2"], env=env)
        assert result.returncode != 0 and b"in development" in result.stderr, result

        # ---- custom install root and previous-root fallback on Windows and Linux
        # Portable profile keeps test data away from the user's real games.
        config = data / "config/launcher.json"
        config.parent.mkdir(parents=True, exist_ok=True)
        root_a = work / "Games A"
        root_b = work / "Games B"
        a_sly2, b_sly2 = root_a / "sly2", root_b / "sly2"
        shutil.copytree(sly2, a_sly2)
        shutil.copytree(sly2, b_sly2)
        iso_library = work / "Disc library"
        iso_library.mkdir()
        config.write_text(json.dumps({
            "install_dir": str(root_b),
            "install_roots": [str(root_a)],
            "iso_library_dir": str(iso_library),
        }))
        result = run([copied, "--installs"], env=env)
        assert result.returncode == 0, result
        listed = result.stdout.decode().splitlines()
        assert any("sly2 usa" in line and str(b_sly2) in line for line in listed), listed
        assert not any("sly2 usa" in line and str(a_sly2) in line for line in listed), listed
        shutil.rmtree(b_sly2)
        result = run([copied, "--installs"], env=env)
        listed = result.stdout.decode().splitlines()
        assert result.returncode == 0 and any("sly2 usa" in line and str(a_sly2) in line for line in listed), listed
        shutil.rmtree(a_sly2)
        result = run([copied, "--installs"], env=env)
        listed = result.stdout.decode().splitlines()
        assert result.returncode == 0 and any("sly2 usa" in line and str(sly2) in line for line in listed), listed
        assert json.loads(config.read_text())["iso_library_dir"] == str(iso_library)
        config.unlink()
        if os.name != "nt":
            runtime = bin_dir / "Sly2-Release"
            runtime.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$4/runtime-args.txt"\npwd >> "$4/runtime-args.txt"\n')
            runtime.chmod(0o755)
            result = run([copied, "--play", "sly2"], env=env)
            assert result.returncode == 0, result
            lines = (data / "user/sly2/runtime-args.txt").read_text().splitlines()
            default_args = ["--data-dir", str(sly2), "--user-dir", str(data / "user/sly2")]
            assert lines == default_args + [str(bin_dir)], lines
            # Sly 2 is the native port: the old emulated runtime's preview option adds
            # nothing, and neither does the removed "Widescreen backgrounds" toggle's
            # key (2026-11-05): an old launcher.json with it off still loads, the game
            # gets no --wide-backgrounds, and the launcher keeps the configuration as it is.
            config = data / "config/launcher.json"
            for selection, extra in (({"sly2_native_preview": True, "last_game": "sly1"}, []),
                                     ({"sly2_wide_backgrounds": False, "last_game": "sly1"}, [])):
                config.write_text(json.dumps(selection))
                for _ in range(2):
                    result = run([copied, "--play", "sly2"], env=env)
                    assert result.returncode == 0, result
                    lines = (data / "user/sly2/runtime-args.txt").read_text().splitlines()
                    assert lines == default_args + extra + [str(bin_dir)], lines
                    assert json.loads(config.read_text()) == selection
            selection = {"sly2_native_preview": True, "last_game": "sly1"}
            config.write_text(json.dumps(selection))
            assert not (data / "installations/sly1/installation.json").exists()
            sly1 = data / "installations/sly1"
            sly1.mkdir(parents=True)
            (sly1 / "installation.json").write_text(json.dumps({
                "schema": 1, "asset_version": 2,
                "disc_sha256": "d4ba3ce30047ffb2a825ef13d37a48c4df63aa57977157a194f66bd0b859796c"}))
            shutil.copy2(runtime, bin_dir / "Sly1-Release")
            result = run([copied, "--play", "sly1"], env=env)
            assert result.returncode == 0, result
            lines = (data / "user/runtime-args.txt").read_text().splitlines()
            assert lines == ["--data-dir", str(sly1), "--user-dir", str(data / "user"), str(bin_dir)], lines
            assert json.loads(config.read_text()) == selection
            # ---- regions side by side (installations/sly1 = the first region, sly1-<region> the others)
            pal_sha = "cfab20b47a0b9ae1c9eb47d278a48704ca8e272505594c6705de1525a7cccb91"
            sly1_eu = data / "installations/sly1-europe"
            sly1_eu.mkdir(parents=True)
            (sly1_eu / "installation.json").write_text(json.dumps({
                "schema": 1, "asset_version": 2, "region": "PAL", "disc_sha256": pal_sha}))
            result = run([copied, "--installs"], env=env)
            assert result.returncode == 0, result
            listed = result.stdout.decode().splitlines()
            assert any(l.startswith("sly1 usa * USA ready") for l in listed), listed
            assert any(l.startswith("sly1 europe   Europe ready") for l in listed), listed
            result = run([copied, "--play", "sly1", "--region", "europe"], env=env)
            assert result.returncode == 0, result
            lines = (data / "user/runtime-args.txt").read_text().splitlines()
            assert lines == ["--data-dir", str(sly1_eu), "--user-dir", str(data / "user"), str(bin_dir)], lines
            assert json.loads(config.read_text())["regions"] == {"sly1": "europe"}
            # the last region played is remembered: plain --play (the Steam shortcut) starts it
            result = run([copied, "--play", "sly1"], env=env)
            lines = (data / "user/runtime-args.txt").read_text().splitlines()
            assert result.returncode == 0 and lines[1] == str(sly1_eu), lines
            result = run([copied, "--play", "sly1", "--region", "korea"], env=env)
            assert result.returncode != 0 and b"no Korea installation" in result.stderr, result
            # an installation in an older asset format asks for a re-extract, and is never started
            (sly1 / "installation.json").write_text(json.dumps({
                "schema": 1, "asset_version": 1,
                "disc_sha256": "d4ba3ce30047ffb2a825ef13d37a48c4df63aa57977157a194f66bd0b859796c"}))
            result = run([copied, "--installs"], env=env)
            assert b"sly1 usa   USA outdated" in result.stdout, result
            result = run([copied, "--play", "sly1", "--region", "usa"], env=env)
            assert result.returncode != 0 and b"changed the format" in result.stderr, result
            (sly1 / "installation.json").write_text(json.dumps({
                "schema": 1, "asset_version": 2,
                "disc_sha256": "d4ba3ce30047ffb2a825ef13d37a48c4df63aa57977157a194f66bd0b859796c"}))
            shutil.rmtree(sly1_eu)
            result = run([copied, "--play", "sly1"], env=env)
            lines = (data / "user/runtime-args.txt").read_text().splitlines()
            assert result.returncode == 0 and lines[1] == str(sly1), lines
            selection = {"sly2_native_preview": True, "last_game": "sly1"}
            config.write_text(json.dumps(selection))
            # Turning the option off, or reading an invalid value from an
            # edited configuration, retains the original default command.
            for value in (False, "true", 1, None, [], {}):
                selection["sly2_native_preview"] = value
                config.write_text(json.dumps(selection))
                result = run([copied, "--play", "sly2"], env=env)
                assert result.returncode == 0, result
                lines = (data / "user/sly2/runtime-args.txt").read_text().splitlines()
                assert lines == default_args + [str(bin_dir)], (value, lines)
                assert json.loads(config.read_text()) == selection
        # ---- user-selected install root, without moving or hiding legacy game files
        if os.name != "nt":
            config = data / "config/launcher.json"
            custom = work / "Games on another drive"
            custom_sly2 = custom / "sly2"
            shutil.copytree(sly2, custom_sly2)
            iso_library = work / "Disc library"
            iso_library.mkdir()
            selection = {"install_dir": str(custom), "iso_library_dir": str(iso_library)}
            config.write_text(json.dumps(selection))
            result = run([copied, "--installs"], env=env)
            assert result.returncode == 0, result
            entries = result.stdout.decode().splitlines()
            assert any("sly2 usa" in line and str(custom_sly2) in line for line in entries), entries
            assert not any("sly2 usa" in line and str(sly2) in line for line in entries), entries
            assert any("sly1 usa" in line and str(sly1) in line for line in entries), entries
            result = run([copied, "--play", "sly2"], env=env)
            assert result.returncode == 0, result
            lines = (data / "user/sly2/runtime-args.txt").read_text().splitlines()
            assert lines[:2] == ["--data-dir", str(custom_sly2)], lines
            # Removing a custom installation reveals the unchanged original.
            shutil.rmtree(custom_sly2)
            result = run([copied, "--installs"], env=env)
            assert result.returncode == 0 and str(sly2).encode() in result.stdout, result
            result = run([copied, "--play", "sly2"], env=env)
            assert result.returncode == 0, result
            lines = (data / "user/sly2/runtime-args.txt").read_text().splitlines()
            assert lines[:2] == ["--data-dir", str(sly2)], lines
            config.write_text("{}")

        result = run([copied, "--play", "sly3"], env=env)
        assert result.returncode != 0, result
        # ---- Sly 3 launch contract (the same as Sly 2's, its own folders)
        sly3 = data / "installations/sly3"
        sly3.mkdir(parents=True)
        (sly3 / "disc.iso").write_bytes(b"disc")
        (sly3 / "installation.json").write_text(json.dumps({
            "schema": 1, "game": "sly3", "asset_version": 1,
            "disc_sha1": "5941b7877e6bf5dfb8acb2d04a3c5059367f385b",
            "files": [{"path": "disc.iso", "size": 4,
                       "sha256": "c7f4f3f6ad3b6e8cb6a2e0b6ff41ee28cbdd88e0f1d1a36e9ce3d1b8e0e1b2e1"}]}))
        result = run([copied, "--play", "sly3"], env=env)
        assert result.returncode != 0 and b"in development" in result.stderr, result
        if os.name != "nt":
            shutil.copy2(runtime, bin_dir / "Sly3-Release")
            result = run([copied, "--play", "sly3"], env=env)
            assert result.returncode == 0, result
            lines = (data / "user/sly3/runtime-args.txt").read_text().splitlines()
            assert lines == ["--data-dir", str(sly3), "--user-dir", str(data / "user/sly3"), str(bin_dir)], lines

        # ---- real discs
        if a.sly1_iso:
            assert identify(extractor, a.sly1_iso)["game"] == "sly1"
            result = run([extractor, "--iso", a.sly1_iso, "--game", "sly2", "--validate-only"])
            assert result.returncode != 0 and b"not Sly 2" in result.stderr, result
        if a.sly3_iso:
            found = identify(extractor, a.sly3_iso)
            assert (found["kind"], found["game"], found["boot_file"]) == ("supported", "sly3", "SCUS_974.64"), found
            result = run([extractor, "--iso", a.sly3_iso, "--game", "sly3", "--validate-only", "--progress=jsonl"])
            assert result.returncode == 0, result.stderr
            result = run([extractor, "--iso", a.sly3_iso, "--validate-only"])
            assert result.returncode == 0, "Sly 3 disc not routed to its installer by default"
            if a.extract:
                shutil.copy2(extractor, bin_dir / extractor.name)
                shutil.rmtree(sly3)
                result = run([copied, "--install", a.sly3_iso], env=env, timeout=3600)
                assert result.returncode == 0, result.stderr[-2000:]
                manifest = json.loads((sly3 / "installation.json").read_text())
                assert manifest["game"] == "sly3" and manifest["image"]["sectors"] == 1735232, manifest
                assert (sly3 / "disc.iso").stat().st_size == a.sly3_iso.stat().st_size == manifest["image"]["size"]
                for name in ("SYSTEM.CNF", "SCUS_974.64", "IOPRP300.IMG"):
                    assert (sly3 / name).is_file(), name
                result = run([copied, "--verify", "sly3"], env=env, timeout=3600)
                assert result.returncode == 0 and b"Installation verified" in result.stdout, result
                print("Sly 3 installed and verified:", sly3)
        if a.sly2_iso:
            found = identify(extractor, a.sly2_iso)
            assert (found["kind"], found["game"], found["boot_file"]) == ("supported", "sly2", "SCUS_973.16"), found
            result = run([extractor, "--iso", a.sly2_iso, "--game", "sly2", "--validate-only", "--progress=jsonl"])
            assert result.returncode == 0, result.stderr
            assert b'"stage":"validate"' in result.stdout
            result = run([extractor, "--iso", a.sly2_iso, "--validate-only"])
            assert result.returncode == 0, "Sly 2 disc not routed to its installer by default"
            if a.extract:
                shutil.rmtree(sly2)
                shutil.copy2(extractor, bin_dir / extractor.name)
                result = run([copied, "--install", a.sly2_iso], env=env, timeout=3600)
                assert result.returncode == 0, result.stderr[-2000:]
                manifest = json.loads((sly2 / "installation.json").read_text())
                assert manifest["game"] == "sly2" and manifest["image"]["sectors"] == 1408272, manifest
                assert (sly2 / "disc.iso").stat().st_size == a.sly2_iso.stat().st_size == manifest["image"]["size"]
                for name in ("SYSTEM.CNF", "SCUS_973.16", "IOPRP300.IMG"):
                    assert (sly2 / name).is_file(), name
                assert b"de2af92a54de71e33edc8a160c31c0337a95e6f5" in (sly2 / "installation.json").read_bytes()
                result = run([copied, "--verify", "sly2"], env=env, timeout=3600)
                assert result.returncode == 0 and b"Installation verified" in result.stdout, result
                print("Sly 2 installed and verified:", sly2)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("Launcher game contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
