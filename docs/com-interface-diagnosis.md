# COM interface diagnostic record

The reported remote DLL returns `E_NOINTERFACE` for `IInitializeWithStream` and
`IThumbnailProvider`. The remote DLL and SDK libraries were not available here,
so their actual symbols and byte values cannot be established from this machine.

Evidence gathered on the local build machine:

- `git ls-tree -r HEAD --name-only` contains no DLL, OBJ, LIB, or TLOG files.
  `.gitignore` excludes `x64/`. An old `C:\Users\admin\Desktop\Portfolio`
  path in a local `link.command.1.tlog` was retained when the working tree was
  moved to `D:\Gitsources`; that ignored local file is not evidence of Git
  tracking a build artifact.
- Local `link.read.1.tlog` names
  `D:\Windows Kits\10\Lib\10.0.22621.0\um\x64\uuid.lib`. `dumpbin /all` on
  that library locates `IID_IInitializeWithStream` in its
  `propsys\uuid\guids.obj` archive member, and `IID_IThumbnailProvider` in
  `shell\published\uuid\shguids.obj`. This identifies the local SDK's symbol
  providers, not the values or provenance of the remote DLL.
- `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\com_interfaces.ps1`
  succeeds locally for both exact published IIDs. The new DLL contains their
  little-endian GUID bytes `9DB424B8AC226141AC8A9916E8FA3F7F` and
  `CDFC57E395A97645B01F234630154E96`.

Remediation: the handler compares the two published IID constants directly,
without linking those comparisons to SDK `IID_*` variables. Packaging always
performs a fresh x64 Release rebuild and runs the COM interface test on both
the new build and the exact copy inside the ZIP. `BUILDINFO.txt` records the
packaged DLL SHA256. On a failing machine, run `verify-com.ps1` from the
extracted ZIP before installation; compare its DLL hash with `BUILDINFO.txt`
and then compare the installed DLL hash with the same value. To determine the
precise cause of the previous remote failure, retain its failing DLL, hash,
and matching build/link logs rather than relying on timestamps alone.
