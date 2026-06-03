---
name: Windows VM with TwinCAT (for test PLC deployments)
description: How to reach and drive the Windows/TwinCAT VM from this Linux host
type: reference
---

The user has a Windows VM with TwinCAT XAE running on the LAN (actually on
this host via VirtualBox NAT, accessible by port-forward).

- SSH: `ssh -p 2222 adminlocal@10.0.2.2`
- VM internal IP (for ADS): `10.0.2.15`
- Git root on VM: `C:\Users\adminlocal\Documents\GIT`
- Default shell on SSH: `cmd.exe`

Pre-built tools on the VM under
`C:\Users\adminlocal\Documents\GIT\PLC_Framework_To_TC3_Build_Test\windows_tools\`:
- `TcBuild.exe` — build a TwinCAT solution via DTE COM
- `TcRun.exe` — activate + run PLC for N seconds
- `TcTest.exe` — build/deploy/run tests via ADS
- `AdsStateSwitch.exe` / `AdsTest.exe` — raw ADS state read / switch

Orchestration script on Linux:
`/home/admin/Desktop/Projects/Frameworks_For_PLC/PLC_Framework_To_TC3_Build_Test/remote_build.sh`

ScopeAnalyser test-PLC projects live OUTSIDE the ScopeAnalyser repo (e.g.
`/home/admin/Desktop/Projects/ScopeAnalyser_TestPlc/`) to keep the
"no PLC code in the product" rule clean.

**Open question:** ADS from this Linux host to the VM's TwinCAT runtime
needs port 48898/TCP forwarded (host→guest) or the VM on a bridged
network. Not yet verified.
