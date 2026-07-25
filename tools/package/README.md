# Debian package validation

`validate_deb.py` performs a local, non-installing validation of the
CardputerZero package:

- Debian control fields, `arm64` architecture, canonical filename, and
  payload `md5sums`
- safe archive paths and regular-file/directory types only; no traversal,
  links, device nodes, setuid/setgid files, maintainer scripts, or systemd
  units
- the required APPLaunch desktop entry, launcher, executable, icons, fonts,
  documentation, and license notices
- `Terminal=false`, fixed safe `Exec`/`Icon` values, executable permissions,
  and a launcher that byte-for-byte matches the generated safe three-line
  wrapper
- ELF64 AArch64 identity, the glibc ARM64 interpreter, absence of
  RPATH/RUNPATH, and a report of all `NEEDED` libraries
- package byte size and SHA-256

Run it directly after packaging:

```sh
python3 tools/package/validate_deb.py \
  dist/lora-messenger_0.1.0-1_arm64.deb
```

The CPack post-build hook runs the same command automatically, so a rejected
package also makes `cpack --preset cp0-cross-deb` fail. This check does not
install or execute the ARM64 application and makes no hardware or radio claim.
An `example.invalid` Maintainer is reported as a publication warning rather
than a local-package error. Before any separately approved publication, replace it
with a user-verified identity, rebuild, and require the strict gate:

```sh
python3 tools/package/validate_deb.py \
  --require-publishable-maintainer \
  dist/lora-messenger_0.1.0-1_arm64.deb
```

The strict gate requires `Verified name <email>` syntax and rejects reserved
placeholder domains. It makes the checked-in `example.invalid` value fail
mechanically; it cannot prove ownership of a replacement email, so that remains a
human approval gate.

Run the validator regression suite with:

```sh
python3 -m unittest -v tools/package/test_validate_deb.py
```

The negative tests construct deterministic Debian `ar` archives in temporary
directories using only the Python standard library. They never extract the
crafted traversal, link, or device-node members and require no network access.
