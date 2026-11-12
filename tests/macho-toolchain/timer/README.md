# timer

`timertest`: a dispatch timer (first at 2 s, interval 2 s, leeway 100 ms) counted over 11 s, and `mach_absolute_time` against the uptime clock (they agree: the timers of libdispatch use deadlines of that clock and the kernel turns them into times from now).  Build: `sh build.sh OUT`.  It fires 7 times instead of 5 (leeway and re-arming: not a runaway); written while looking for what keeps the real `launchservicesd` busy (it counts its Mach ports in a loop: `mach_port_names` + `mach_port_get_refs` for each, about 300 calls a second, one core).
