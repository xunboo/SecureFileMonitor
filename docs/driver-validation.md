# Manual driver validation

The source and binaries are ready for development testing. Building successfully is not proof that a kernel driver is safe to deploy. Use a disposable Windows 10 2004-or-later / Windows 11 x64 VM with a snapshot and disposable files. No driver has been installed or loaded as part of this coding session.

## Build and signing

Build `SecureFileMonitor.sln` in VS 2026, Release | x64, or run:

```powershell
.\scripts\Build.ps1 -Configuration Release
```

The driver project deliberately produces an **unsigned** `.sys`. Windows x64 will not load it as an ordinary unsigned kernel driver. Use Microsoft's [test-signing instructions](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing) and [TESTSIGNING policy documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option) to prepare your test VM. Secure Boot, organizational policy, and memory integrity can affect the permitted signing workflow. The scripts do not modify those settings.

For a development certificate and embedded signature:

```powershell
.\scripts\Sign-Driver.ps1 -Configuration Release
```

This creates a code-signing certificate in **CurrentUser\My**, signs the built SYS using `signtool`, and exports its public certificate next to the SYS. To use an existing certificate instead, supply `-CertificateThumbprint`. Keep the private key private. In your test VM, establish the certificate trust and signing policy required by the Microsoft guide. The installer checks `Get-AuthenticodeSignature` and refuses an invalid/untrusted signature.

The included INF passes the WDK's current INF checks and uses the Windows 11 24H2+ `Parameters\Instances` layout. It names a catalog for a future packaged/signable INF distribution. The current build does not generate or sign a catalog: the development workflow below installs the **embedded-signed SYS directly**, registers both instance layouts, and does not use PnP package installation. For a distributable driver package, generate/sign the catalog and follow Microsoft's production signing requirements.

The altitude **370030.42 is only a development placeholder**. Check for collisions in your VM and use `-Altitude` when needed. Obtain your own [Microsoft-assigned minifilter altitude](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/minifilter-altitude-request) before any production distribution.

## Install, load and remove

Run an elevated PowerShell on the prepared test machine:

```powershell
# Refuses to overwrite an existing service/driver file.
.\scripts\Install-Driver.ps1 -Configuration Release

fltmc load SecureFileMonitor
fltmc filters
fltmc instances -f SecureFileMonitor
```

The service is demand-start and does not automatically enable protection. Launch `out\x64\Release\SecureFileMonitor.App\SecureFileMonitor.App.exe` **as administrator**, configure a disposable local NTFS file, and click **Start monitoring**.

To unload/remove after testing:

```powershell
# Prefer Stop monitoring / Exit in the tray menu first.
fltmc unload SecureFileMonitor
.\scripts\Uninstall-Driver.ps1
```

The uninstall script does not delete user settings/logs, certificates, or change the Windows signing policy. If loading fails, inspect Event Viewer → Microsoft → Windows → CodeIntegrity → Operational. Missing certificate trust or an unapproved kernel signing policy is different from a driver code failure.

## Suggested functional checks

Create a small text file in a scratch directory, add it to Protected files, and use the included I/O probe. `write` **appends a line** and requires an existing file.

```powershell
.\out\x64\Release\IoProbe\IoProbe.exe read C:\SfmTest\sample.txt
.\out\x64\Release\IoProbe\IoProbe.exe write C:\SfmTest\sample.txt
```

| Check | Expected result |
|---|---|
| No rule; block the open | Probe gets Windows error 5; a blocked event is logged |
| No rule; leave the prompt unanswered | Access denied around its 20-second deadline; timeout reason logged |
| Allow with process permission | Following reads from that same PID/creation time and approved access type are decided automatically |
| Allow read / deny write rules for IoProbe | Read succeeds; an append or write-capable open is denied |
| Same executable basename in a different directory | Does not inherit a rule for the first executable's full path |
| Save an executable/file rule | Rule persists across app restart and is used on subsequent matching operations |
| Named data stream of the selected file | Uses the selected base-file policy; event path includes the stream |
| Similar sibling name, e.g. `sample.txt.bak` | Not matched merely because it shares a prefix |
| Close prompt or press Escape | Current operation denied, without implicitly saving a rule |
| Stop monitoring | New operations proceed without this filter's access checks |
| Close main window | Tray remains; reopen from its icon |
| Export CSV after clearing the view | Full session's received events remain in the exported file |
| Allow an operation that the file system rejects | Log shows Allow plus the real failure NTSTATUS |

Check existing handles too:

```powershell
# Start this while monitoring is stopped. Enable monitoring before pressing Enter.
.\out\x64\Release\IoProbe\IoProbe.exe read C:\SfmTest\sample.txt --pause-before-io
```

This should exercise interception of a read using an already-open handle, subject to the documented safe-defer constraints.

## Reliability and coverage checks

- Repeat with Notepad and another editor. Expect programs to request broader access than the visible action suggests; compare the desired-access mask in JSONL.
- Verify high-DPI/multiple-monitor rendering, keyboard-only navigation, long Unicode paths, notification settings, minimize/restore, Explorer restart, and multiple pending decisions.
- Exercise concurrent requests, process exit while prompted, canceled I/O, monitor Stop with pending work, driver unload, repeated connect/disconnect, disk-full logging, and broker termination. The app must report faults and known audit gaps instead of displaying successful protection.
- Killing the broker while monitoring is enabled intentionally leaves matched requests denied by the loaded driver. Recover by reconnecting or unloading the filter. Do this only with disposable files.
- Stress beyond 16 outstanding decisions and the ring-buffer capacity. Verify overload denials and `lost_events` records. Record any timing/stability problems before expanding usage.
- Test memory-mapped I/O, hard links, case-sensitive paths, file-ID opens, rename/replacement and metadata behavior to confirm the limits in the README. These are not covered by an assertion that every possible access is enforced.
- Use Microsoft's [Driver Verifier guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/driver-verifier) and a kernel debugger in the VM for pool, IRQL, I/O verification and unload/rundown testing. Keep a VM recovery route available.

The included native tests validate user-mode policy and storage logic. They are not a substitute for these kernel/runtime checks.
