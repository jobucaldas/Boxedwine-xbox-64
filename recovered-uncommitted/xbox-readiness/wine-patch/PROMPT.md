# Task: make Wine 10.0's own Unix-side ntdll obey two Xbox-UWP constraints (W^X, no /proc), prove on Linux

Context: research project to run Windows x64 games inside a UWP app on the user's own Xbox in Developer Mode, using Wine's own loader in-process and only documented UWP capabilities. Two UWP rules matter here, and making Wine comply with them makes it STRICTER/safer, not less:
 (a) W^X: UWP's VirtualProtectFromApp (codeGeneration capability) rejects PAGE_EXECUTE_READWRITE; code pages must be written as RW then flipped to RX.
 (b) No /proc or /sys in a UWP app.
We emulate them on Linux with /workspace/xbox-readiness/bin/uwpjail (seccomp: mmap/mprotect with PROT_WRITE|PROT_EXEC -> EACCES; see /workspace/xbox-readiness/src/uwpjail.c) and with bwrap without /proc and /sys (see /workspace/xbox-readi
