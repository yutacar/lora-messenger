#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

from __future__ import annotations

import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("validate_deb.py")
PROJECT_ROOT = MODULE_PATH.parents[2]
SPEC = importlib.util.spec_from_file_location("validate_deb", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)

DPKG_DEB = shutil.which("dpkg-deb")
CMAKE = shutil.which("cmake")


def regular_member(name: str, data: bytes, mode: int = 0o644) -> tuple[tarfile.TarInfo, bytes]:
    member = tarfile.TarInfo(name)
    member.mode = mode
    member.uid = 0
    member.gid = 0
    member.uname = "root"
    member.gname = "root"
    member.mtime = 0
    member.size = len(data)
    return member, data


def special_member(
    name: str,
    member_type: bytes,
    *,
    mode: int = 0o644,
    linkname: str = "",
) -> tuple[tarfile.TarInfo, None]:
    member = tarfile.TarInfo(name)
    member.type = member_type
    member.mode = mode
    member.uid = 0
    member.gid = 0
    member.uname = "root"
    member.gname = "root"
    member.mtime = 0
    member.linkname = linkname
    if member_type in {tarfile.CHRTYPE, tarfile.BLKTYPE}:
        member.devmajor = 1
        member.devminor = 3
    return member, None


def compressed_tar(
    members: list[tuple[tarfile.TarInfo, bytes | None]],
) -> bytes:
    """Create a reproducible gzip-compressed tar stream without extracting it."""

    uncompressed = io.BytesIO()
    with tarfile.open(
        fileobj=uncompressed,
        mode="w",
        format=tarfile.GNU_FORMAT,
    ) as archive:
        for member, data in members:
            archive.addfile(
                member,
                io.BytesIO(data) if data is not None else None,
            )
    return gzip.compress(uncompressed.getvalue(), compresslevel=9, mtime=0)


def ar_member(name: str, data: bytes) -> bytes:
    stored_name = f"{name}/"
    if len(stored_name) > 16:
        raise ValueError(f"ar fixture member name is too long: {name}")
    header = (
        f"{stored_name:<16}"
        f"{0:<12}"
        f"{0:<6}"
        f"{0:<6}"
        f"{0o100644:<8o}"
        f"{len(data):<10}"
        "`\n"
    ).encode("ascii")
    if len(header) != 60:
        raise AssertionError(f"invalid ar header length: {len(header)}")
    return header + data + (b"\n" if len(data) % 2 else b"")


def minimal_aarch64_elf() -> bytes:
    identification = b"\x7fELF\x02\x01\x01" + (b"\x00" * 9)
    return struct.pack(
        "<16sHHIQQQIHHHHHH",
        identification,
        3,
        183,
        1,
        0,
        0,
        0,
        0,
        64,
        56,
        0,
        64,
        0,
        0,
    )


def png_header(width: int, height: int) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + struct.pack(
        ">I4sII",
        13,
        b"IHDR",
        width,
        height,
    )


def baseline_payload(
    wrapper: bytes,
) -> list[tuple[tarfile.TarInfo, bytes | None]]:
    package = "lora-messenger"
    app_root = "usr/share/APPLaunch"
    files: list[tuple[str, bytes, int]] = [
        (f"{app_root}/bin/{package}", minimal_aarch64_elf(), 0o755),
        (f"{app_root}/bin/{package}-launch", wrapper, 0o755),
        (
            f"{app_root}/applications/{package}.desktop",
            (
                "[Desktop Entry]\n"
                "Type=Application\n"
                "Name=LoRa Messenger\n"
                f"Exec=/usr/share/APPLaunch/bin/{package}-launch\n"
                f"Icon=share/images/{package}.png\n"
                "Terminal=false\n"
            ).encode(),
            0o644,
        ),
        (f"{app_root}/share/images/{package}.png", png_header(100, 100), 0o644),
        (f"{app_root}/share/images/{package}_100.png", png_header(100, 100), 0o644),
        (f"{app_root}/share/images/{package}_80.png", png_header(80, 80), 0o644),
        (f"usr/share/{package}/fonts/inter-medium.ttf", b"fixture-font\n", 0o644),
        (f"usr/share/{package}/fonts/inter-regular.ttf", b"fixture-font\n", 0o644),
        (f"usr/share/{package}/fonts/lora-ui-ja.otf", b"fixture-font\n", 0o644),
        (f"usr/share/{package}/fonts/lora-ui-zh-hans.otf", b"fixture-font\n", 0o644),
        (f"usr/share/{package}/licenses/THIRD_PARTY_NOTICES.md", b"notices\n", 0o644),
        (f"usr/share/{package}/licenses/MIT.txt", b"license\n", 0o644),
        (f"usr/share/doc/{package}/copyright", b"copyright\n", 0o644),
        (f"usr/share/doc/{package}/README.md", b"readme\n", 0o644),
    ]
    return [
        regular_member(f"./{path}", data, mode)
        for path, data, mode in files
    ]


def build_deb_fixture(
    directory: Path,
    *,
    extra_payload: list[tuple[tarfile.TarInfo, bytes | None]] | None = None,
    extra_control: list[tuple[tarfile.TarInfo, bytes | None]] | None = None,
    wrapper: bytes | None = None,
) -> Path:
    package = "lora-messenger"
    if wrapper is None:
        wrapper = (
            "#!/usr/bin/env sh\n"
            "set -eu\n"
            f'exec /usr/share/APPLaunch/bin/{package} "$@"\n'
        ).encode()
    payload_members = baseline_payload(wrapper)
    payload_members.extend(extra_payload or [])

    md5_lines: list[str] = []
    for member, data in payload_members:
        if not member.isreg() or data is None:
            continue
        try:
            normalized = VALIDATOR.normalize_tar_path(member.name)
        except ValueError:
            continue
        digest = hashlib.md5(data, usedforsecurity=False).hexdigest()
        md5_lines.append(f"{digest}  {normalized}\n")

    control = (
        "Package: lora-messenger\n"
        "Version: 0.1.0-1\n"
        "Architecture: arm64\n"
        "Maintainer: Fixture Maintainer <fixture@project.example.jp>\n"
        "Description: Deterministic validator fixture\n"
    ).encode()
    control_members: list[tuple[tarfile.TarInfo, bytes | None]] = [
        regular_member("./control", control),
        regular_member("./md5sums", "".join(sorted(md5_lines)).encode()),
    ]
    control_members.extend(extra_control or [])

    archive = (
        b"!<arch>\n"
        + ar_member("debian-binary", b"2.0\n")
        + ar_member("control.tar.gz", compressed_tar(control_members))
        + ar_member("data.tar.gz", compressed_tar(payload_members))
    )
    package_path = directory / "lora-messenger_0.1.0-1_arm64.deb"
    package_path.write_bytes(archive)
    return package_path


def validate_fixture(
    package_path: Path,
    *,
    sysroot: Path | None = None,
    readelf: str | None = None,
) -> "VALIDATOR.Validation":
    assert DPKG_DEB is not None
    validation, _report = VALIDATOR.validate_package(
        package_path,
        dpkg_deb=DPKG_DEB,
        readelf=readelf,
        sysroot=sysroot,
        expected_package="lora-messenger",
        expected_architecture="arm64",
        require_publishable_maintainer=True,
    )
    return validation


class ValidatorPrimitiveTests(unittest.TestCase):
    def test_parse_deb822_continuation(self) -> None:
        fields = VALIDATOR.parse_deb822(
            "Package: lora-messenger\n"
            "Version: 0.1.0-1\n"
            "Description: Summary\n"
            " continuation\n"
        )
        self.assertEqual(fields["Package"], "lora-messenger")
        self.assertEqual(fields["Description"], "Summary\ncontinuation")

    def test_parse_deb822_rejects_duplicate_field(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate"):
            VALIDATOR.parse_deb822("Package: first\nPackage: second\n")

    def test_normalize_tar_path(self) -> None:
        self.assertEqual(
            VALIDATOR.normalize_tar_path("./usr/share/APPLaunch/bin/app"),
            "usr/share/APPLaunch/bin/app",
        )

    def test_normalize_tar_path_rejects_unsafe_paths(self) -> None:
        for path in ("/usr/bin/app", "../usr/bin/app", "usr/../bin/app", "usr\\bin"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                VALIDATOR.normalize_tar_path(path)

    def test_parse_elf_identity_accepts_aarch64(self) -> None:
        data = bytearray(64)
        data[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<H", data, 18, 183)
        self.assertEqual(VALIDATOR.parse_elf_identity(bytes(data)), (2, 1, 183))

    def test_parse_elf_identity_reports_other_machine(self) -> None:
        data = bytearray(64)
        data[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<H", data, 18, 62)
        self.assertEqual(VALIDATOR.parse_elf_identity(bytes(data)), (2, 1, 62))

    def test_parse_png_dimensions(self) -> None:
        data = b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sII", 13, b"IHDR", 100, 80)
        self.assertEqual(VALIDATOR.parse_png_dimensions(data), (100, 80))

    def test_canonical_deb_filename(self) -> None:
        self.assertEqual(
            VALIDATOR.expected_deb_filename("lora-messenger", "0.1.0-1", "arm64"),
            "lora-messenger_0.1.0-1_arm64.deb",
        )

    def test_publishable_maintainer_accepts_named_non_reserved_email(self) -> None:
        self.assertIsNone(
            VALIDATOR.publishable_maintainer_problem(
                "LoRa Messenger Maintainer <maintainer@project.example.jp>"
            )
        )

    def test_publishable_maintainer_rejects_placeholder_and_ambiguous_forms(
        self,
    ) -> None:
        cases = (
            "LoRa Messenger contributors <noreply@example.invalid>",
            "Maintainer <person@example.com>",
            "nobody@project.example.jp",
            "Placeholder <person@project.example.jp>",
        )
        for maintainer in cases:
            with self.subTest(maintainer=maintainer):
                self.assertIsNotNone(
                    VALIDATOR.publishable_maintainer_problem(maintainer)
                )

    def test_parse_semantic_symbol_versions(self) -> None:
        self.assertEqual(
            VALIDATOR.parse_symbol_version("GLIBCXX_3.4.29"),
            ("GLIBCXX", (3, 4, 29)),
        )
        self.assertEqual(
            VALIDATOR.parse_symbol_version("CXXABI_1.3.15"),
            ("CXXABI", (1, 3, 15)),
        )
        self.assertEqual(
            VALIDATOR.parse_symbol_version("GLIBC_2.34"),
            ("GLIBC", (2, 34)),
        )
        self.assertIsNone(VALIDATOR.parse_symbol_version("GLIBC_PRIVATE"))
        self.assertIsNone(VALIDATOR.parse_symbol_version("GCC_12.0"))

    def test_symbol_version_maxima_are_semantic(self) -> None:
        maxima = VALIDATOR.symbol_version_maxima(
            {"GLIBC_2.9", "GLIBC_2.34", "GLIBCXX_3.4.9", "GLIBCXX_3.4.29"}
        )
        self.assertEqual(maxima["GLIBC"], "GLIBC_2.34")
        self.assertEqual(maxima["GLIBCXX"], "GLIBCXX_3.4.29")

    def test_symbol_version_set_comparison(self) -> None:
        missing, newer = VALIDATOR.compare_symbol_version_sets(
            {"GLIBCXX_3.4.19", "GLIBCXX_3.4.34", "GLIBCXX_3.4"},
            {"GLIBCXX_3.4", "GLIBCXX_3.4.20", "GLIBCXX_3.4.33"},
        )
        self.assertEqual(missing, {"GLIBCXX_3.4.19", "GLIBCXX_3.4.34"})
        self.assertEqual(newer, {"GLIBCXX_3.4.34"})

    def test_parse_readelf_version_sets(self) -> None:
        needs = VALIDATOR.parse_version_needs(
            "Version needs section '.gnu.version_r' contains 2 entries:\n"
            "  000000: Version: 1  File: libstdc++.so.6  Cnt: 2\n"
            "  0x0010: Name: GLIBCXX_3.4.29 Flags: none Version: 4\n"
            "  0x0020: Name: CXXABI_1.3.15 Flags: none Version: 3\n"
            "  0x0030: Version: 1  File: libc.so.6  Cnt: 1\n"
            "  0x0040: Name: GLIBC_2.34 Flags: none Version: 2\n"
        )
        self.assertEqual(
            needs["libstdc++.so.6"],
            {"GLIBCXX_3.4.29", "CXXABI_1.3.15"},
        )
        self.assertEqual(needs["libc.so.6"], {"GLIBC_2.34"})
        definitions = VALIDATOR.parse_version_definitions(
            "Version definition section '.gnu.version_d' contains 2 entries:\n"
            "  0x001c: Rev: 1 Flags: none Index: 2 Cnt: 1 Name: GLIBCXX_3.4\n"
            "  0x0038: Rev: 1 Flags: none Index: 3 Cnt: 1 Name: GLIBCXX_3.4.29\n"
            "Version needs section '.gnu.version_r' contains 0 entries:\n"
        )
        self.assertEqual(definitions, {"GLIBCXX_3.4", "GLIBCXX_3.4.29"})


@unittest.skipUnless(DPKG_DEB is not None, "dpkg-deb is required")
class DebianFixtureRejectionTests(unittest.TestCase):
    def assert_validation_error(
        self,
        validation: "VALIDATOR.Validation",
        expected: str,
    ) -> None:
        self.assertTrue(
            any(expected in error for error in validation.errors),
            f"missing {expected!r} in validation errors: {validation.errors}",
        )

    def test_rejects_payload_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory(prefix="lora-deb-traversal-") as temporary:
            package = build_deb_fixture(
                Path(temporary),
                extra_payload=[regular_member("../outside", b"escape\n")],
            )
            validation = validate_fixture(package)
        self.assert_validation_error(validation, "path traversal in archive member")

    def test_rejects_symbolic_and_hard_links(self) -> None:
        cases = (
            (
                "symbolic",
                special_member(
                    "./usr/share/lora-messenger/link",
                    tarfile.SYMTYPE,
                    linkname="/etc/passwd",
                ),
            ),
            (
                "hard",
                special_member(
                    "./usr/share/lora-messenger/hardlink",
                    tarfile.LNKTYPE,
                    linkname="./usr/share/lora-messenger/licenses/MIT.txt",
                ),
            ),
        )
        for label, member in cases:
            with self.subTest(link_type=label):
                with tempfile.TemporaryDirectory(prefix=f"lora-deb-{label}-") as temporary:
                    package = build_deb_fixture(
                        Path(temporary),
                        extra_payload=[member],
                    )
                    validation = validate_fixture(package)
                self.assert_validation_error(
                    validation,
                    "unsupported payload member type",
                )

    def test_rejects_device_node(self) -> None:
        with tempfile.TemporaryDirectory(prefix="lora-deb-device-") as temporary:
            package = build_deb_fixture(
                Path(temporary),
                extra_payload=[
                    special_member(
                        "./usr/share/lora-messenger/device",
                        tarfile.CHRTYPE,
                    )
                ],
            )
            validation = validate_fixture(package)
        self.assert_validation_error(validation, "unsupported payload member type")

    def test_rejects_setuid_and_setgid_modes(self) -> None:
        for label, mode in (("setuid", 0o4755), ("setgid", 0o2755)):
            with self.subTest(mode=label):
                with tempfile.TemporaryDirectory(prefix=f"lora-deb-{label}-") as temporary:
                    package = build_deb_fixture(
                        Path(temporary),
                        extra_payload=[
                            regular_member(
                                f"./usr/share/APPLaunch/bin/{label}-fixture",
                                b"not executable\n",
                                mode,
                            )
                        ],
                    )
                    validation = validate_fixture(package)
                self.assert_validation_error(
                    validation,
                    "setuid/setgid payload member",
                )

    def test_rejects_systemd_unit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="lora-deb-systemd-") as temporary:
            package = build_deb_fixture(
                Path(temporary),
                extra_payload=[
                    regular_member(
                        "./lib/systemd/system/lora-messenger.service",
                        b"[Service]\nExecStart=/bin/true\n",
                    )
                ],
            )
            validation = validate_fixture(package)
        self.assert_validation_error(validation, "systemd unit/path is forbidden")

    def test_rejects_control_script(self) -> None:
        with tempfile.TemporaryDirectory(prefix="lora-deb-control-") as temporary:
            package = build_deb_fixture(
                Path(temporary),
                extra_control=[
                    regular_member(
                        "./postinst",
                        b"#!/bin/sh\nexit 0\n",
                        0o755,
                    )
                ],
            )
            validation = validate_fixture(package)
        self.assert_validation_error(
            validation,
            "maintainer/control script is forbidden",
        )

    def test_rejects_modified_wrapper(self) -> None:
        wrapper = (
            "#!/usr/bin/env sh\n"
            "set -eu\n"
            "printf audit >/dev/null\n"
            'exec /usr/share/APPLaunch/bin/lora-messenger "$@"\n'
        ).encode()
        with tempfile.TemporaryDirectory(prefix="lora-deb-wrapper-") as temporary:
            package = build_deb_fixture(Path(temporary), wrapper=wrapper)
            validation = validate_fixture(package)
        self.assert_validation_error(
            validation,
            "must exactly match the generated safe three-line template",
        )

    def test_rejects_sysroot_symlink_escape(self) -> None:
        readelf = VALIDATOR.find_readelf(None)
        if readelf is None:
            self.skipTest("readelf is required")
        with tempfile.TemporaryDirectory(prefix="lora-deb-sysroot-") as temporary:
            root = Path(temporary)
            package = build_deb_fixture(root)
            sysroot = root / "sysroot"
            library_directory = sysroot / "usr/lib/aarch64-linux-gnu"
            library_directory.mkdir(parents=True)
            outside_library = root / "outside-libstdc++.so.6"
            outside_library.write_bytes(b"outside sysroot\n")
            try:
                (library_directory / "libstdc++.so.6").symlink_to(outside_library)
            except OSError as error:
                self.skipTest(f"cannot create fixture symlink: {error}")
            validation = validate_fixture(
                package,
                sysroot=sysroot,
                readelf=readelf,
            )
        self.assert_validation_error(
            validation,
            "sysroot library symlink escapes target sysroot",
        )

    @unittest.skipUnless(CMAKE is not None, "cmake is required")
    def test_cpack_hook_propagates_validator_failure(self) -> None:
        wrapper = (
            "#!/usr/bin/env sh\n"
            "set -eu\n"
            "true\n"
            'exec /usr/share/APPLaunch/bin/lora-messenger "$@"\n'
        ).encode()
        with tempfile.TemporaryDirectory(prefix="lora-deb-cpack-") as temporary:
            package = build_deb_fixture(Path(temporary), wrapper=wrapper)
            result = subprocess.run(
                [
                    CMAKE,
                    f"-DCPACK_PACKAGE_FILES={package}",
                    f"-DCPACK_PACKAGE_VALIDATOR={MODULE_PATH}",
                    "-DCPACK_DEBIAN_PACKAGE_NAME=lora-messenger",
                    "-DCPACK_DEBIAN_PACKAGE_ARCHITECTURE=arm64",
                    "-P",
                    str(PROJECT_ROOT / "cmake/validate-deb-package.cmake"),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("Debian package validation failed", output)
        self.assertIn(
            "launcher wrapper must exactly match the generated safe three-line",
            output,
        )


@unittest.skipUnless(CMAKE is not None, "cmake is required")
class PackageMetadataGateTests(unittest.TestCase):
    expected_permissions = {
        "keyboard_input": True,
        "network": False,
        "filesystem": "app-data-only",
        "external_hardware": False,
        "hdmi_output": False,
        "background_service": False,
        "audio_output": False,
        "microphone": False,
        "camera": False,
        "sensors": False,
        "gps": False,
        "device_id": False,
    }

    def current_manifest(self) -> dict[str, object]:
        return json.loads((PROJECT_ROOT / "app-builder.json").read_text())

    def metadata_fixture(
        self,
        directory: Path,
        manifest: dict[str, object],
    ) -> None:
        store = manifest["store"]
        assert isinstance(store, dict)
        referenced_assets = [store["icon"], *store["screenshots"]]
        for relative in [
            "cmake/templates/app.desktop.in",
            "cmake/cm0-package.cmake",
            *referenced_assets,
        ]:
            source = PROJECT_ROOT / relative
            destination = directory / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        (directory / "app-builder.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
        )

    def run_metadata_gate(
        self,
        directory: Path,
    ) -> subprocess.CompletedProcess[str]:
        assert CMAKE is not None
        return subprocess.run(
            [
                CMAKE,
                f"-DSOURCE_ROOT={directory}",
                "-P",
                str(PROJECT_ROOT / "cmake/validate-package-metadata.cmake"),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )

    def test_manifest_has_exact_approved_permission_map(self) -> None:
        manifest = self.current_manifest()
        store = manifest["store"]
        assert isinstance(store, dict)
        self.assertEqual(store["permissions"], self.expected_permissions)

    def test_gate_rejects_unexpected_permission_key(self) -> None:
        manifest = self.current_manifest()
        store = manifest["store"]
        assert isinstance(store, dict)
        permissions = store["permissions"]
        assert isinstance(permissions, dict)
        permissions["unexpected"] = False
        with tempfile.TemporaryDirectory(prefix="lora-metadata-extra-") as temporary:
            root = Path(temporary)
            self.metadata_fixture(root, manifest)
            result = self.run_metadata_gate(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("must contain exactly the 12 approved keys", output)

    def test_gate_rejects_permission_value_change(self) -> None:
        manifest = self.current_manifest()
        store = manifest["store"]
        assert isinstance(store, dict)
        permissions = store["permissions"]
        assert isinstance(permissions, dict)
        permissions["microphone"] = True
        with tempfile.TemporaryDirectory(prefix="lora-metadata-value-") as temporary:
            root = Path(temporary)
            self.metadata_fixture(root, manifest)
            result = self.run_metadata_gate(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("store.permissions.microphone", output)


if __name__ == "__main__":
    unittest.main()
