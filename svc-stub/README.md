# svc-stub

`svc-stub NAME`: a service that answers every request with the plain success (the id plus 100, size 0x28, return code 0).  For services of the Mac that a program only needs to hear "done" from (for example `com.apple.PowerManagement.control`, the `io_pm_*` routines of IOKit).  Build: `sh build.sh OUT`.
