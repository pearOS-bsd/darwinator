# macprobe: what the real macOS says

`probe.c` prints the sizes of structures, the values of constants, the `sysctl` values and the results of some calls on a real Mac: the things that `darwinator` and `machrun` had to assume.
`probe.sh` builds it, runs it, and (only if you answer `y`) traces the system calls of the applications you name with `dtruss`.

    sh probe.sh                                              # only the probe
    sh probe.sh /Applications/Keka.app /Applications/AppCleaner.app   # and a trace of these, 8 seconds each (sudo)

Everything is written to `./macprobe-out` and `macprobe-out.tar.gz`; nothing leaves the machine by itself.  The result is compared with `tests/c/syscalls.c` and the headers of `src/darwin_abi.h`.
