#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate the LoRa Messenger CardputerZero Debian package without installing it."""

from __future__ import annotations

import argparse
import configparser
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from typing import BinaryIO, Iterable


DEFAULT_PACKAGE = "lora-messenger"
DEFAULT_ARCHITECTURE = "arm64"
EXPECTED_INTERPRETER = "/lib/ld-linux-aarch64.so.1"
MAX_ARCHIVE_MEMBERS = 10_000
MAX_UNPACKED_BYTES = 512 * 1024 * 1024
MAX_CAPTURE_BYTES = 256 * 1024 * 1024

CONTROL_SCRIPTS = {
    "config",
    "preinst",
    "postinst",
    "prerm",
    "postrm",
}
SYSTEMD_SUFFIXES = {
    ".service",
    ".socket",
    ".timer",
    ".path",
    ".target",
    ".mount",
    ".automount",
    ".slice",
    ".scope",
}
SHELL_METACHARACTERS = re.compile(r"[;&|`$<>\r\n]")
DEBIAN_VERSION = re.compile(r"^[0-9][A-Za-z0-9.+:~\-]*$")
MD5_LINE = re.compile(r"^([0-9a-fA-F]{32})[ \t]+(?:\*)?(.+)$")
NEEDED_LINE = re.compile(
    r"\(NEEDED\).*Shared library:\s*\[([^\]]+)\]",
    re.IGNORECASE,
)
INTERPRETER_LINE = re.compile(
    r"Requesting program interpreter:\s*([^\]]+)",
    re.IGNORECASE,
)
VERSION_FILE_LINE = re.compile(r"\bFile:\s*(\S+)")
VERSION_NAME_LINE = re.compile(r"\bName:\s*(\S+)")
SEMANTIC_SYMBOL_VERSION = re.compile(
    r"^(GLIBCXX|CXXABI|GLIBC)_(\d+(?:\.\d+)*)$"
)
MAINTAINER_IDENTITY = re.compile(
    r"^\s*([^<>\r\n]+?)\s*<([^<>\s@]+)@([^<>\s@]+)>\s*$"
)
RESERVED_MAINTAINER_DOMAINS = {
    "example.com",
    "example.net",
    "example.org",
    "example.invalid",
    "invalid",
    "localhost",
}
TRACKED_VERSION_PREFIXES = ("GLIBCXX_", "CXXABI_", "GLIBC_")
VERSION_PREFIX_ORDER = {
    "GLIBCXX": 0,
    "CXXABI": 1,
    "GLIBC": 2,
}
SYSROOT_LIBRARY_DIRS = (
    "usr/lib/aarch64-linux-gnu",
    "lib/aarch64-linux-gnu",
    "usr/aarch64-linux-gnu/lib",
    "aarch64-linux-gnu/lib",
    "usr/lib",
    "lib",
)


class Validation:
    """Accumulate independent validation findings for one package."""

    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def require(self, condition: bool, message: str) -> bool:
        if not condition:
            self.errors.append(message)
            return False
        return True

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)


def parse_deb822(text: str) -> dict[str, str]:
    """Parse the single RFC822-like control stanza emitted by dpkg-deb."""

    fields: dict[str, str] = {}
    current: str | None = None
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not line:
            current = None
            continue
        if line[0] in " \t":
            if current is None:
                raise ValueError(
                    f"continuation line {line_number} has no preceding field"
                )
            fields[current] += "\n" + line[1:]
            continue
        if ":" not in line:
            raise ValueError(f"control line {line_number} has no ':' separator")
        key, value = line.split(":", 1)
        if not key or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*", key):
            raise ValueError(f"invalid control field name on line {line_number}")
        if key in fields:
            raise ValueError(f"duplicate control field: {key}")
        fields[key] = value.lstrip()
        current = key
    return fields


def normalize_tar_path(raw_name: str) -> str:
    """Return a safe package-relative POSIX path or raise ValueError."""

    if not raw_name:
        raise ValueError("empty archive member name")
    if "\x00" in raw_name:
        raise ValueError("NUL byte in archive member name")
    if "\\" in raw_name:
        raise ValueError(f"backslash in archive member name: {raw_name!r}")
    path = PurePosixPath(raw_name)
    if path.is_absolute():
        raise ValueError(f"absolute archive member path: {raw_name!r}")

    parts: list[str] = []
    for part in raw_name.split("/"):
        if part in {"", "."}:
            continue
        if part == "..":
            raise ValueError(f"path traversal in archive member: {raw_name!r}")
        parts.append(part)
    return "/".join(parts)


def expected_deb_filename(package: str, version: str, architecture: str) -> str:
    return f"{package}_{version}_{architecture}.deb"


def publishable_maintainer_problem(maintainer: str) -> str | None:
    """Return why a Maintainer is not suitable for an approved publication."""

    match = MAINTAINER_IDENTITY.fullmatch(maintainer)
    if match is None:
        return "Maintainer must use the form 'Verified name <email@example>'"
    display_name, _local_part, domain = match.groups()
    if not display_name.strip():
        return "Maintainer display name must be present"
    normalized_domain = domain.rstrip(".").lower()
    if (
        normalized_domain in RESERVED_MAINTAINER_DOMAINS
        or normalized_domain.endswith(".invalid")
        or normalized_domain.endswith(".test")
        or normalized_domain.endswith(".localhost")
    ):
        return f"Maintainer uses a reserved placeholder domain: {domain}"
    if "placeholder" in maintainer.casefold():
        return "Maintainer contains placeholder text"
    return None


def parse_png_dimensions(data: bytes) -> tuple[int, int]:
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    if data[12:16] != b"IHDR":
        raise ValueError("PNG does not start with an IHDR chunk")
    width, height = struct.unpack(">II", data[16:24])
    if width == 0 or height == 0:
        raise ValueError("PNG has a zero dimension")
    return width, height


def parse_elf_identity(data: bytes) -> tuple[int, int, int]:
    """Return (class, data encoding, machine) from the ELF identification."""

    if len(data) < 20 or data[:4] != b"\x7fELF":
        raise ValueError("file is not ELF")
    elf_class = data[4]
    data_encoding = data[5]
    if elf_class not in {1, 2}:
        raise ValueError(f"unknown ELF class: {elf_class}")
    if data_encoding == 1:
        byte_order = "<"
    elif data_encoding == 2:
        byte_order = ">"
    else:
        raise ValueError(f"unknown ELF data encoding: {data_encoding}")
    machine = struct.unpack_from(byte_order + "H", data, 18)[0]
    return elf_class, data_encoding, machine


def parse_symbol_version(name: str) -> tuple[str, tuple[int, ...]] | None:
    """Parse a numeric glibc/libstdc++ symbol version for semantic ordering."""

    match = SEMANTIC_SYMBOL_VERSION.fullmatch(name)
    if not match:
        return None
    return match.group(1), tuple(int(component) for component in match.group(2).split("."))


def _version_sort_key(name: str) -> tuple[int, tuple[int, ...], str]:
    parsed = parse_symbol_version(name)
    if parsed is None:
        return len(VERSION_PREFIX_ORDER), (), name
    prefix, version = parsed
    return VERSION_PREFIX_ORDER[prefix], version, name


def symbol_version_maxima(versions: Iterable[str]) -> dict[str, str]:
    """Return the semantically newest numeric version for each tracked prefix."""

    maxima: dict[str, tuple[tuple[int, ...], str]] = {}
    for name in versions:
        parsed = parse_symbol_version(name)
        if parsed is None:
            continue
        prefix, version = parsed
        current = maxima.get(prefix)
        if current is None or version > current[0]:
            maxima[prefix] = (version, name)
    return {prefix: value[1] for prefix, value in maxima.items()}


def compare_symbol_version_sets(
    required: set[str],
    provided: set[str],
) -> tuple[set[str], set[str]]:
    """Return (missing exact versions, versions newer than provider maxima)."""

    missing = required - provided
    provided_maxima: dict[str, tuple[int, ...]] = {}
    for name in provided:
        parsed = parse_symbol_version(name)
        if parsed is None:
            continue
        prefix, version = parsed
        current = provided_maxima.get(prefix)
        if current is None or version > current:
            provided_maxima[prefix] = version

    newer: set[str] = set()
    for name in required:
        parsed = parse_symbol_version(name)
        if parsed is None:
            continue
        prefix, version = parsed
        provided_maximum = provided_maxima.get(prefix)
        if provided_maximum is not None and version > provided_maximum:
            newer.add(name)
    return missing, newer


def parse_version_needs(readelf_output: str) -> dict[str, set[str]]:
    """Parse tracked symbol-version requirements grouped by DT_NEEDED library."""

    needs: dict[str, set[str]] = {}
    in_needs = False
    current_library: str | None = None
    for line in readelf_output.splitlines():
        if line.startswith("Version needs section"):
            in_needs = True
            current_library = None
            continue
        if line.startswith(("Version definition section", "Version symbols section")):
            in_needs = False
            current_library = None
            continue
        if not in_needs:
            continue
        file_match = VERSION_FILE_LINE.search(line)
        if file_match:
            current_library = file_match.group(1)
            needs.setdefault(current_library, set())
            continue
        name_match = VERSION_NAME_LINE.search(line)
        if name_match and current_library is not None:
            name = name_match.group(1)
            if name.startswith(TRACKED_VERSION_PREFIXES):
                needs[current_library].add(name)
    return needs


def parse_version_definitions(readelf_output: str) -> set[str]:
    """Parse tracked symbol-version definitions from a shared object."""

    definitions: set[str] = set()
    in_definitions = False
    for line in readelf_output.splitlines():
        if line.startswith("Version definition section"):
            in_definitions = True
            continue
        if line.startswith(("Version needs section", "Version symbols section")):
            in_definitions = False
            continue
        if not in_definitions:
            continue
        match = VERSION_NAME_LINE.search(line)
        if match and match.group(1).startswith(TRACKED_VERSION_PREFIXES):
            definitions.add(match.group(1))
    return definitions


def _md5() -> "hashlib._Hash":
    try:
        return hashlib.md5(usedforsecurity=False)
    except TypeError:  # pragma: no cover - compatibility with older Python
        return hashlib.md5()


def _stream_member(
    archive: tarfile.TarFile,
    member: tarfile.TarInfo,
    *,
    capture: bool,
    validation: Validation,
) -> tuple[str, bytes | None]:
    stream = archive.extractfile(member)
    if stream is None:
        validation.error(f"cannot read regular archive member: {member.name!r}")
        return "", None

    digest = _md5()
    captured = bytearray() if capture else None
    read_size = 0
    while True:
        chunk = stream.read(1024 * 1024)
        if not chunk:
            break
        read_size += len(chunk)
        digest.update(chunk)
        if captured is not None:
            if read_size > MAX_CAPTURE_BYTES:
                validation.error(
                    f"required member is too large to inspect safely: {member.name!r}"
                )
                captured = None
            else:
                captured.extend(chunk)
    if read_size != member.size:
        validation.error(
            f"archive member size mismatch for {member.name!r}: "
            f"header={member.size}, read={read_size}"
        )
    return digest.hexdigest(), bytes(captured) if captured is not None else None


def _open_deb_tar(
    dpkg_deb: str,
    switch: str,
    package_path: Path,
) -> tuple[subprocess.Popen[bytes], tarfile.TarFile]:
    process = subprocess.Popen(
        [dpkg_deb, switch, os.fspath(package_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None
    try:
        archive = tarfile.open(fileobj=process.stdout, mode="r|*")
    except Exception:
        process.stdout.close()
        process.kill()
        process.wait()
        raise
    return process, archive


def _finish_deb_tar(
    process: subprocess.Popen[bytes],
    archive: tarfile.TarFile,
    *,
    label: str,
    validation: Validation,
) -> None:
    archive.close()
    if process.stdout is not None:
        process.stdout.close()
    stderr = b""
    if process.stderr is not None:
        stderr = process.stderr.read()
        process.stderr.close()
    return_code = process.wait()
    if return_code != 0:
        detail = stderr.decode("utf-8", errors="replace").strip()
        validation.error(
            f"dpkg-deb failed while reading {label} (exit {return_code})"
            + (f": {detail}" if detail else "")
        )


def inspect_control_archive(
    dpkg_deb: str,
    package_path: Path,
    validation: Validation,
) -> dict[str, bytes]:
    captured: dict[str, bytes] = {}
    seen: set[str] = set()
    member_count = 0
    try:
        process, archive = _open_deb_tar(
            dpkg_deb, "--ctrl-tarfile", package_path
        )
    except (OSError, tarfile.TarError) as error:
        validation.error(f"cannot open Debian control archive: {error}")
        return captured

    try:
        for member in archive:
            member_count += 1
            if member_count > MAX_ARCHIVE_MEMBERS:
                validation.error(
                    f"control archive exceeds {MAX_ARCHIVE_MEMBERS} members"
                )
                break
            try:
                path = normalize_tar_path(member.name)
            except ValueError as error:
                validation.error(str(error))
                continue
            if not path:
                continue
            if "/" in path:
                validation.error(f"nested control archive member: {path!r}")
            if path in seen:
                validation.error(f"duplicate control archive member: {path!r}")
                continue
            seen.add(path)

            mode = member.mode & 0o7777
            if mode & 0o6000:
                validation.error(
                    f"setuid/setgid control archive member: {path!r}"
                )
            if not (member.isdir() or member.isreg()):
                validation.error(
                    f"unsupported control archive member type: {path!r}"
                )
                continue
            if member.isreg() and mode & 0o111:
                validation.error(f"executable control archive member: {path!r}")
            if member.isreg() and (
                path in CONTROL_SCRIPTS or path.endswith(".sh")
            ):
                validation.error(f"maintainer/control script is forbidden: {path!r}")
            if member.isreg() and path in {"control", "md5sums"}:
                _, content = _stream_member(
                    archive,
                    member,
                    capture=True,
                    validation=validation,
                )
                if content is not None:
                    captured[path] = content
    except (OSError, tarfile.TarError) as error:
        validation.error(f"cannot inspect Debian control archive: {error}")
    finally:
        _finish_deb_tar(
            process,
            archive,
            label="control archive",
            validation=validation,
        )

    validation.require("control" in captured, "control archive has no control file")
    validation.require(
        "md5sums" in captured, "control archive has no md5sums file"
    )
    return captured


def _allowed_payload_path(path: str, package: str) -> bool:
    roots = (
        "usr/share/APPLaunch",
        f"usr/share/{package}",
        f"usr/share/doc/{package}",
    )
    parents = {
        "usr",
        "usr/share",
        "usr/share/doc",
    }
    return path in parents or any(path == root or path.startswith(root + "/") for root in roots)


def _is_systemd_unit(path: str) -> bool:
    suffix = PurePosixPath(path).suffix
    return "/systemd/" in f"/{path}" or suffix in SYSTEMD_SUFFIXES


def inspect_payload_archive(
    dpkg_deb: str,
    package_path: Path,
    package: str,
    capture_paths: set[str],
    validation: Validation,
) -> tuple[set[str], dict[str, tarfile.TarInfo], dict[str, str], dict[str, bytes]]:
    paths: set[str] = set()
    members: dict[str, tarfile.TarInfo] = {}
    md5_by_path: dict[str, str] = {}
    captured: dict[str, bytes] = {}
    member_count = 0
    unpacked_size = 0
    try:
        process, archive = _open_deb_tar(
            dpkg_deb, "--fsys-tarfile", package_path
        )
    except (OSError, tarfile.TarError) as error:
        validation.error(f"cannot open Debian payload archive: {error}")
        return paths, members, md5_by_path, captured

    try:
        for member in archive:
            member_count += 1
            if member_count > MAX_ARCHIVE_MEMBERS:
                validation.error(
                    f"payload archive exceeds {MAX_ARCHIVE_MEMBERS} members"
                )
                break
            try:
                path = normalize_tar_path(member.name)
            except ValueError as error:
                validation.error(str(error))
                continue
            if not path:
                continue
            if path in paths:
                validation.error(f"duplicate payload archive member: {path!r}")
                continue
            paths.add(path)
            members[path] = member

            if not _allowed_payload_path(path, package):
                validation.error(f"payload path is outside allowed trees: {path!r}")
            if _is_systemd_unit(path):
                validation.error(f"systemd unit/path is forbidden: {path!r}")

            mode = member.mode & 0o7777
            if mode & 0o6000:
                validation.error(f"setuid/setgid payload member: {path!r}")
            if mode & 0o002:
                validation.error(f"world-writable payload member: {path!r}")
            if not (member.isdir() or member.isreg()):
                validation.error(
                    f"unsupported payload member type (links/devices/FIFOs are "
                    f"forbidden): {path!r}"
                )
                continue
            if member.isreg() and mode & 0o111 and not path.startswith(
                "usr/share/APPLaunch/bin/"
            ):
                validation.error(
                    f"executable payload file is outside APPLaunch/bin: {path!r}"
                )
            if member.isreg():
                unpacked_size += member.size
                if unpacked_size > MAX_UNPACKED_BYTES:
                    validation.error(
                        f"payload exceeds {MAX_UNPACKED_BYTES} unpacked bytes"
                    )
                    break
                digest, content = _stream_member(
                    archive,
                    member,
                    capture=path in capture_paths,
                    validation=validation,
                )
                md5_by_path[path] = digest
                if content is not None:
                    captured[path] = content
    except (OSError, tarfile.TarError) as error:
        validation.error(f"cannot inspect Debian payload archive: {error}")
    finally:
        _finish_deb_tar(
            process,
            archive,
            label="payload archive",
            validation=validation,
        )

    return paths, members, md5_by_path, captured


def parse_md5sums(data: bytes, validation: Validation) -> dict[str, str]:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        validation.error(f"md5sums is not UTF-8: {error}")
        return {}

    result: dict[str, str] = {}
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not line:
            continue
        match = MD5_LINE.fullmatch(line)
        if not match:
            validation.error(f"invalid md5sums line {line_number}: {line!r}")
            continue
        try:
            path = normalize_tar_path(match.group(2))
        except ValueError as error:
            validation.error(f"invalid md5sums path on line {line_number}: {error}")
            continue
        if path in result:
            validation.error(f"duplicate md5sums entry: {path!r}")
            continue
        result[path] = match.group(1).lower()
    return result


def validate_md5sums(
    expected: dict[str, str],
    actual: dict[str, str],
    validation: Validation,
) -> None:
    for path in sorted(expected.keys() - actual.keys()):
        validation.error(f"md5sums references missing payload file: {path!r}")
    for path in sorted(actual.keys() - expected.keys()):
        validation.error(f"regular payload file missing from md5sums: {path!r}")
    for path in sorted(expected.keys() & actual.keys()):
        if expected[path] != actual[path]:
            validation.error(
                f"payload MD5 mismatch for {path!r}: "
                f"expected {expected[path]}, got {actual[path]}"
            )


def validate_required_layout(
    package: str,
    paths: set[str],
    members: dict[str, tarfile.TarInfo],
    validation: Validation,
) -> dict[str, str]:
    app_root = "usr/share/APPLaunch"
    required_files = {
        "binary": f"{app_root}/bin/{package}",
        "wrapper": f"{app_root}/bin/{package}-launch",
        "desktop": f"{app_root}/applications/{package}.desktop",
        "icon": f"{app_root}/share/images/{package}.png",
        "icon_100": f"{app_root}/share/images/{package}_100.png",
        "icon_80": f"{app_root}/share/images/{package}_80.png",
        "font_medium": f"usr/share/{package}/fonts/inter-medium.ttf",
        "font_regular": f"usr/share/{package}/fonts/inter-regular.ttf",
        "font_ja": f"usr/share/{package}/fonts/lora-ui-ja.otf",
        "font_zh": f"usr/share/{package}/fonts/lora-ui-zh-hans.otf",
        "copyright": f"usr/share/doc/{package}/copyright",
        "readme": f"usr/share/doc/{package}/README.md",
        "notices": f"usr/share/{package}/licenses/THIRD_PARTY_NOTICES.md",
    }
    for label, path in required_files.items():
        if validation.require(path in paths, f"missing required {label}: /{path}"):
            validation.require(
                members[path].isreg(), f"required {label} is not a regular file: /{path}"
            )
            validation.require(
                members[path].size > 0, f"required {label} is empty: /{path}"
            )

    for label in ("binary", "wrapper"):
        path = required_files[label]
        if path in members and members[path].isreg():
            validation.require(
                bool(members[path].mode & 0o111),
                f"required {label} is not executable: /{path}",
            )

    license_prefix = f"usr/share/{package}/licenses/"
    license_files = [
        path
        for path, member in members.items()
        if path.startswith(license_prefix) and member.isreg()
    ]
    validation.require(
        len(license_files) >= 2,
        f"runtime license tree must contain notices and license texts: "
        f"/{license_prefix}",
    )
    return required_files


def _decode_utf8(label: str, data: bytes, validation: Validation) -> str | None:
    if b"\x00" in data:
        validation.error(f"{label} contains a NUL byte")
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        validation.error(f"{label} is not UTF-8: {error}")
        return None


def validate_desktop_entry(
    data: bytes,
    package: str,
    validation: Validation,
) -> None:
    text = _decode_utf8("desktop entry", data, validation)
    if text is None:
        return
    parser = configparser.ConfigParser(
        interpolation=None,
        strict=True,
        delimiters=("=",),
        comment_prefixes=("#",),
    )
    parser.optionxform = str
    try:
        parser.read_string(text)
    except configparser.Error as error:
        validation.error(f"invalid desktop entry: {error}")
        return
    if not validation.require(
        parser.has_section("Desktop Entry"),
        "desktop entry has no [Desktop Entry] section",
    ):
        return
    entry = parser["Desktop Entry"]
    validation.require(entry.get("Type") == "Application", "desktop Type must be Application")
    validation.require(bool(entry.get("Name", "").strip()), "desktop Name must be non-empty")
    validation.require(
        entry.get("Terminal", "").lower() == "false",
        "desktop Terminal must be false",
    )

    expected_exec = f"/usr/share/APPLaunch/bin/{package}-launch"
    exec_value = entry.get("Exec", "")
    validation.require(exec_value == expected_exec, f"desktop Exec must be {expected_exec}")
    validation.require(
        not SHELL_METACHARACTERS.search(exec_value),
        "desktop Exec contains shell metacharacters",
    )
    validation.require(
        not any(character.isspace() for character in exec_value),
        "desktop Exec must not contain arguments or whitespace",
    )

    expected_icon = f"share/images/{package}.png"
    icon_value = entry.get("Icon", "")
    validation.require(icon_value == expected_icon, f"desktop Icon must be {expected_icon}")
    validation.require(
        ".." not in PurePosixPath(icon_value).parts
        and not PurePosixPath(icon_value).is_absolute()
        and not SHELL_METACHARACTERS.search(icon_value),
        "desktop Icon path is unsafe",
    )


def validate_wrapper(data: bytes, package: str, validation: Validation) -> None:
    text = _decode_utf8("launcher wrapper", data, validation)
    if text is None:
        return
    expected = (
        "#!/usr/bin/env sh\n"
        "set -eu\n"
        f'exec /usr/share/APPLaunch/bin/{package} "$@"\n'
    )
    validation.require(
        text == expected,
        "launcher wrapper must exactly match the generated safe three-line template",
    )

    shell = shutil.which("sh")
    if shell is None:
        validation.error("cannot find sh for launcher syntax validation")
        return
    with tempfile.NamedTemporaryFile(prefix="lora-launch-", suffix=".sh") as script:
        script.write(data)
        script.flush()
        result = subprocess.run(
            [shell, "-n", script.name],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    if result.returncode != 0:
        detail = result.stderr.strip()
        validation.error(
            "launcher wrapper fails sh -n"
            + (f": {detail}" if detail else "")
        )


def find_readelf(requested: str | None) -> str | None:
    if requested:
        if os.path.sep in requested:
            return requested if os.path.isfile(requested) else None
        return shutil.which(requested)
    for name in (
        "aarch64-linux-gnu-readelf",
        "aarch64-unknown-linux-gnu-readelf",
        "llvm-readelf",
        "readelf",
    ):
        resolved = shutil.which(name)
        if resolved:
            return resolved
    return None


def _readelf(
    executable: str,
    flag: str,
    elf_path: str,
    label: str,
    validation: Validation,
) -> str | None:
    result = subprocess.run(
        [executable, flag, elf_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip()
        validation.error(
            f"readelf failed for {label} (exit {result.returncode})"
            + (f": {detail}" if detail else "")
        )
        return None
    return result.stdout


def validate_elf(
    data: bytes,
    readelf: str | None,
    validation: Validation,
) -> tuple[str | None, list[str]]:
    try:
        elf_class, data_encoding, machine = parse_elf_identity(data)
    except ValueError as error:
        validation.error(f"application binary {error}")
        return None, []
    validation.require(elf_class == 2, "application binary must be ELF64")
    validation.require(
        data_encoding == 1, "application binary must be little-endian ELF"
    )
    validation.require(
        machine == 183,
        f"application binary must target AArch64 (e_machine 183), got {machine}",
    )
    if readelf is None:
        validation.error(
            "no readelf implementation found; install aarch64-linux-gnu-readelf "
            "or pass --readelf"
        )
        return None, []

    with tempfile.NamedTemporaryFile(prefix="lora-elf-", suffix=".bin") as executable:
        executable.write(data)
        executable.flush()
        program_headers = _readelf(
            readelf, "-lW", executable.name, "program headers", validation
        )
        dynamic = _readelf(
            readelf, "-dW", executable.name, "dynamic section", validation
        )

    interpreter: str | None = None
    if program_headers is not None:
        match = INTERPRETER_LINE.search(program_headers)
        if match:
            interpreter = match.group(1).strip()
        validation.require(
            interpreter == EXPECTED_INTERPRETER,
            f"ELF interpreter must be {EXPECTED_INTERPRETER}, got "
            f"{interpreter or '<missing>'}",
        )

    needed: list[str] = []
    if dynamic is not None:
        validation.require(
            "(RPATH)" not in dynamic and "(RUNPATH)" not in dynamic,
            "ELF must not contain RPATH or RUNPATH",
        )
        needed = sorted(set(NEEDED_LINE.findall(dynamic)))
        validation.require(bool(needed), "ELF dynamic section has no NEEDED entries")
        for library in needed:
            validation.require(
                "/" not in library and "\\" not in library,
                f"ELF NEEDED entry contains a path: {library!r}",
            )
    return interpreter, needed


def resolve_sysroot_library(
    sysroot: Path,
    soname: str,
    validation: Validation,
) -> Path | None:
    """Resolve a known AArch64 library path without permitting sysroot escape."""

    try:
        resolved_root = sysroot.expanduser().resolve(strict=True)
    except OSError as error:
        validation.error(f"cannot resolve target sysroot {sysroot}: {error}")
        return None
    if not validation.require(
        resolved_root.is_dir(),
        f"target sysroot is not a directory: {resolved_root}",
    ):
        return None

    for directory in SYSROOT_LIBRARY_DIRS:
        candidate = resolved_root / directory / soname
        if not candidate.exists() and not candidate.is_symlink():
            continue
        try:
            resolved = candidate.resolve(strict=True)
        except OSError as error:
            validation.error(f"cannot resolve sysroot library {candidate}: {error}")
            return None
        try:
            resolved.relative_to(resolved_root)
        except ValueError:
            validation.error(
                f"sysroot library symlink escapes target sysroot: "
                f"{candidate} -> {resolved}"
            )
            return None
        if not validation.require(
            resolved.is_file(),
            f"sysroot library is not a regular file: {resolved}",
        ):
            return None
        try:
            with resolved.open("rb") as library:
                identity = parse_elf_identity(library.read(64))
        except (OSError, ValueError) as error:
            validation.error(f"invalid sysroot library {resolved}: {error}")
            return None
        validation.require(
            identity == (2, 1, 183),
            f"sysroot library must be little-endian ELF64 AArch64: {resolved}",
        )
        return resolved

    searched = ", ".join(f"{directory}/{soname}" for directory in SYSROOT_LIBRARY_DIRS)
    validation.error(
        f"cannot find {soname} in target sysroot {resolved_root}; searched {searched}"
    )
    return None


def validate_abi_versions(
    data: bytes,
    readelf: str,
    sysroot: Path,
    validation: Validation,
) -> dict[str, dict[str, str]]:
    """Compare packaged ELF version needs with target runtime definitions."""

    with tempfile.NamedTemporaryFile(prefix="lora-abi-", suffix=".bin") as executable:
        executable.write(data)
        executable.flush()
        app_versions_output = _readelf(
            readelf,
            "--version-info",
            executable.name,
            "application symbol versions",
            validation,
        )
    if app_versions_output is None:
        return {}

    needs_by_library = parse_version_needs(app_versions_output)
    required_versions = set().union(*needs_by_library.values()) if needs_by_library else set()
    required_by_prefix: dict[str, set[str]] = {
        prefix: set() for prefix in VERSION_PREFIX_ORDER
    }
    for name in required_versions:
        parsed = parse_symbol_version(name)
        if parsed is not None:
            required_by_prefix[parsed[0]].add(name)
            continue
        # Preserve non-numeric tracked versions for exact comparison as well.
        if name.startswith("GLIBCXX_"):
            required_by_prefix["GLIBCXX"].add(name)
        elif name.startswith("CXXABI_"):
            required_by_prefix["CXXABI"].add(name)
        elif name.startswith("GLIBC_"):
            required_by_prefix["GLIBC"].add(name)

    validation.require(
        bool(required_by_prefix["GLIBCXX"]),
        "packaged ELF has no GLIBCXX symbol-version requirements",
    )
    validation.require(
        bool(required_by_prefix["CXXABI"]),
        "packaged ELF has no CXXABI symbol-version requirements",
    )
    validation.require(
        bool(required_by_prefix["GLIBC"]),
        "packaged ELF has no GLIBC symbol-version requirements",
    )

    libraries = {
        "libstdc++.so.6": resolve_sysroot_library(
            sysroot, "libstdc++.so.6", validation
        ),
        "libc.so.6": resolve_sysroot_library(sysroot, "libc.so.6", validation),
    }
    definitions_by_library: dict[str, set[str]] = {}
    for soname, library in libraries.items():
        if library is None:
            continue
        output = _readelf(
            readelf,
            "--version-info",
            os.fspath(library),
            f"{soname} symbol versions",
            validation,
        )
        if output is None:
            continue
        definitions = parse_version_definitions(output)
        validation.require(
            bool(definitions),
            f"target sysroot {soname} exports no tracked symbol versions",
        )
        definitions_by_library[soname] = definitions

    provided_by_prefix = {
        "GLIBCXX": {
            name
            for name in definitions_by_library.get("libstdc++.so.6", set())
            if name.startswith("GLIBCXX_")
        },
        "CXXABI": {
            name
            for name in definitions_by_library.get("libstdc++.so.6", set())
            if name.startswith("CXXABI_")
        },
        "GLIBC": {
            name
            for name in definitions_by_library.get("libc.so.6", set())
            if name.startswith("GLIBC_")
        },
    }

    report: dict[str, dict[str, str]] = {}
    for prefix in VERSION_PREFIX_ORDER:
        required = required_by_prefix[prefix]
        provided = provided_by_prefix[prefix]
        missing, newer = compare_symbol_version_sets(required, provided)
        required_maximum = symbol_version_maxima(required).get(prefix, "<none>")
        provided_maximum = symbol_version_maxima(provided).get(prefix, "<none>")
        report[prefix] = {
            "required_max": required_maximum,
            "provided_max": provided_maximum,
        }
        if missing:
            validation.error(
                f"target sysroot is missing required {prefix} versions: "
                + ", ".join(sorted(missing, key=_version_sort_key))
            )
        if newer:
            validation.error(
                f"packaged ELF requires {prefix} newer than target maximum "
                f"{provided_maximum}: "
                + ", ".join(sorted(newer, key=_version_sort_key))
            )
    return report


def validate_images(
    required: dict[str, str],
    captured: dict[str, bytes],
    validation: Validation,
) -> None:
    expected_sizes = {
        required["icon"]: (100, 100),
        required["icon_100"]: (100, 100),
        required["icon_80"]: (80, 80),
    }
    for path, expected in expected_sizes.items():
        data = captured.get(path)
        if data is None:
            continue
        try:
            dimensions = parse_png_dimensions(data)
        except ValueError as error:
            validation.error(f"/{path}: {error}")
            continue
        validation.require(
            dimensions == expected,
            f"/{path} must be {expected[0]}x{expected[1]}, "
            f"got {dimensions[0]}x{dimensions[1]}",
        )


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as package:
        while chunk := package.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def dpkg_control_fields(
    dpkg_deb: str,
    package_path: Path,
    validation: Validation,
) -> dict[str, str]:
    try:
        result = subprocess.run(
            [dpkg_deb, "--field", os.fspath(package_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    except OSError as error:
        validation.error(f"cannot execute dpkg-deb: {error}")
        return {}
    if result.returncode != 0:
        detail = result.stderr.strip()
        validation.error(
            f"dpkg-deb --field failed (exit {result.returncode})"
            + (f": {detail}" if detail else "")
        )
        return {}
    try:
        return parse_deb822(result.stdout)
    except ValueError as error:
        validation.error(f"invalid Debian control metadata: {error}")
        return {}


def validate_control_metadata(
    package_path: Path,
    fields: dict[str, str],
    expected_package: str,
    expected_architecture: str,
    require_publishable_maintainer: bool,
    validation: Validation,
) -> tuple[str, str, str]:
    package = fields.get("Package", "")
    version = fields.get("Version", "")
    architecture = fields.get("Architecture", "")
    maintainer = fields.get("Maintainer", "")

    validation.require(package == expected_package, f"Package must be {expected_package!r}")
    validation.require(
        bool(DEBIAN_VERSION.fullmatch(version)),
        f"invalid or missing Debian Version: {version!r}",
    )
    validation.require(
        architecture == expected_architecture,
        f"Architecture must be {expected_architecture!r}",
    )
    validation.require(bool(maintainer.strip()), "Maintainer must be present")
    validation.require(
        "\n" not in maintainer and "\r" not in maintainer,
        "Maintainer must be a single line",
    )
    validation.require(
        bool(fields.get("Description", "").strip()),
        "Description must be present",
    )
    maintainer_problem = publishable_maintainer_problem(maintainer)
    if require_publishable_maintainer and maintainer_problem is not None:
        validation.error(
            f"publication-strict Maintainer check failed: {maintainer_problem}"
        )
    elif maintainer_problem is not None:
        validation.warn(
            f"{maintainer_problem}; acceptable for local validation, but "
            "publication-strict validation requires verified identity"
        )

    if package and version and architecture:
        expected_name = expected_deb_filename(package, version, architecture)
        validation.require(
            package_path.name == expected_name,
            f"non-canonical package filename: expected {expected_name!r}, "
            f"got {package_path.name!r}",
        )
    return package or expected_package, version, architecture


def validate_package(
    package_path: Path,
    *,
    dpkg_deb: str,
    readelf: str | None,
    sysroot: Path | None,
    expected_package: str,
    expected_architecture: str,
    require_publishable_maintainer: bool,
) -> tuple[Validation, dict[str, object]]:
    validation = Validation()
    report: dict[str, object] = {}

    if not validation.require(package_path.is_file(), f"package not found: {package_path}"):
        return validation, report
    validation.require(package_path.suffix == ".deb", "package filename must end in .deb")
    report["size_bytes"] = package_path.stat().st_size
    report["sha256"] = file_sha256(package_path)

    fields = dpkg_control_fields(dpkg_deb, package_path, validation)
    package, version, architecture = validate_control_metadata(
        package_path,
        fields,
        expected_package,
        expected_architecture,
        require_publishable_maintainer,
        validation,
    )
    report.update(
        {
            "package": package,
            "version": version,
            "architecture": architecture,
            "maintainer": fields.get("Maintainer", ""),
        }
    )

    control = inspect_control_archive(dpkg_deb, package_path, validation)
    if "control" in control:
        try:
            embedded_fields = parse_deb822(control["control"].decode("utf-8"))
            validation.require(
                embedded_fields == fields,
                "control archive metadata differs from dpkg-deb --field output",
            )
        except (UnicodeDecodeError, ValueError) as error:
            validation.error(f"invalid embedded control file: {error}")

    app_root = "usr/share/APPLaunch"
    binary_path = f"{app_root}/bin/{package}"
    wrapper_path = f"{app_root}/bin/{package}-launch"
    desktop_path = f"{app_root}/applications/{package}.desktop"
    image_paths = {
        f"{app_root}/share/images/{package}.png",
        f"{app_root}/share/images/{package}_100.png",
        f"{app_root}/share/images/{package}_80.png",
    }
    capture_paths = {binary_path, wrapper_path, desktop_path} | image_paths
    paths, members, actual_md5, captured = inspect_payload_archive(
        dpkg_deb,
        package_path,
        package,
        capture_paths,
        validation,
    )
    if "md5sums" in control:
        expected_md5 = parse_md5sums(control["md5sums"], validation)
        validate_md5sums(expected_md5, actual_md5, validation)

    required = validate_required_layout(package, paths, members, validation)
    if desktop_path in captured:
        validate_desktop_entry(captured[desktop_path], package, validation)
    if wrapper_path in captured:
        validate_wrapper(captured[wrapper_path], package, validation)
    validate_images(required, captured, validation)

    interpreter: str | None = None
    needed: list[str] = []
    abi_versions: dict[str, dict[str, str]] = {}
    if binary_path in captured:
        interpreter, needed = validate_elf(
            captured[binary_path],
            readelf,
            validation,
        )
        if sysroot is not None and readelf is not None:
            abi_versions = validate_abi_versions(
                captured[binary_path],
                readelf,
                sysroot,
                validation,
            )
    report["interpreter"] = interpreter or ""
    report["needed"] = needed
    report["abi_versions"] = abi_versions
    report["sysroot"] = os.fspath(sysroot) if sysroot is not None else ""
    report["payload_files"] = len(actual_md5)
    return validation, report


def _resolve_tool(requested: str, description: str) -> str:
    if os.path.sep in requested:
        if os.path.isfile(requested) and os.access(requested, os.X_OK):
            return requested
        raise ValueError(f"{description} is not executable: {requested}")
    resolved = shutil.which(requested)
    if resolved is None:
        raise ValueError(f"cannot find {description}: {requested}")
    return resolved


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate control metadata, archive safety, APPLaunch layout, "
            "assets, and the ARM64 ELF in a LoRa Messenger .deb."
        )
    )
    parser.add_argument("package", type=Path, help="path to the .deb package")
    parser.add_argument(
        "--expected-package",
        default=DEFAULT_PACKAGE,
        help=f"required Debian package name (default: {DEFAULT_PACKAGE})",
    )
    parser.add_argument(
        "--expected-architecture",
        default=DEFAULT_ARCHITECTURE,
        help=f"required Debian architecture (default: {DEFAULT_ARCHITECTURE})",
    )
    parser.add_argument(
        "--dpkg-deb",
        default="dpkg-deb",
        help="dpkg-deb executable or command name",
    )
    parser.add_argument(
        "--readelf",
        help=(
            "readelf executable or command name; defaults to an AArch64-prefixed "
            "implementation, llvm-readelf, or readelf"
        ),
    )
    parser.add_argument(
        "--sysroot",
        type=Path,
        help=(
            "optional target sysroot; compare packaged GLIBCXX/CXXABI/GLIBC "
            "requirements with its AArch64 libstdc++.so.6 and libc.so.6"
        ),
    )
    parser.add_argument(
        "--require-publishable-maintainer",
        action="store_true",
        help=(
            "fail if Maintainer is not in 'Verified name <email>' form or uses "
            "a reserved placeholder domain; required before any approved publish"
        ),
    )
    return parser.parse_args(list(argv))


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    package_path = args.package.expanduser().resolve()
    try:
        dpkg_deb = _resolve_tool(args.dpkg_deb, "dpkg-deb")
    except ValueError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    readelf = find_readelf(args.readelf)
    if args.readelf and readelf is None:
        print(f"ERROR: readelf is not executable or not found: {args.readelf}", file=sys.stderr)
        return 2

    validation, report = validate_package(
        package_path,
        dpkg_deb=dpkg_deb,
        readelf=readelf,
        sysroot=args.sysroot,
        expected_package=args.expected_package,
        expected_architecture=args.expected_architecture,
        require_publishable_maintainer=args.require_publishable_maintainer,
    )

    for warning in validation.warnings:
        print(f"WARNING: {warning}", file=sys.stderr)
    if validation.errors:
        for error in validation.errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print(
            f"Debian package validation FAILED: {len(validation.errors)} error(s)",
            file=sys.stderr,
        )
        return 1

    print(
        f"Debian package valid: {report['package']} "
        f"{report['version']} ({report['architecture']})"
    )
    print(f"  File: {package_path}")
    print(f"  Size: {report['size_bytes']} bytes")
    print(f"  SHA256: {report['sha256']}")
    print(f"  Payload files: {report['payload_files']}")
    print(f"  ELF interpreter: {report['interpreter']}")
    needed = report["needed"]
    assert isinstance(needed, list)
    print(f"  ELF NEEDED: {', '.join(needed)}")
    abi_versions = report["abi_versions"]
    assert isinstance(abi_versions, dict)
    if abi_versions:
        print(f"  ABI sysroot: {report['sysroot']}")
        for prefix in VERSION_PREFIX_ORDER:
            versions = abi_versions[prefix]
            print(
                f"  ABI {prefix}: required max {versions['required_max']}; "
                f"sysroot max {versions['provided_max']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
