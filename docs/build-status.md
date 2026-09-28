# Build verification — 2026-09-27

Verified with Visual Studio 2026, MSVC v145, Windows SDK / WDK 10.0.28000.0, and 64-bit MSBuild.

| Check | Result |
|---|---|
| Entire solution, Debug / x64 | Passed |
| Entire solution, Release / x64 | Passed |
| WDK INF validation | Passed |
| Native Release rule/protocol/storage suite | 23 passed, 0 failed |
| PowerShell helper syntax parsing | Passed |
| App version resource | Secure File Monitor 1.0.0.0 |
| Driver signing state | NotSigned, intentionally unsigned build |
| Driver installation/loading | Not performed |
| GUI inspection and live interception | Not validated; manual testing left to the user |

All projects treat compiler warnings as errors. The unit suite does not simulate or validate kernel enforcement. See [driver-validation.md](driver-validation.md) for runtime checks and [../README.md](../README.md) for coverage limits.

Release SHA-256 hashes (signing the SYS changes its hash):

```text
5D85292C5E2C58C8DF1D9F17D8BB1224D718005EC7E6659ED05D4084DF5E1956  SecureFileMonitor.App.exe
7A755100954585E8C1BA708DB44A26BD03AE643FB5724BD0E367B8DAB1C808FE  SecureFileMonitor.sys
0357E081CB0D473E55A737585C844759C4AC31E67E3A97C895FEE4786E9723B4  IoProbe.exe
```
