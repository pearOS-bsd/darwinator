# signals

`sigtest`: kill() to oneself, alarm() (setitimer) with a sleep, and sigsuspend() woken by an alarm.  Found with it: Darwin's `struct timeval` has a 32 bit `tv_usec` and padding (setitimer failed with EINVAL until the upper half was dropped) and `__pthread_canceled` answered "cancelled" for everything, so every sleep that a signal interrupted ended its thread (exit 0).  Build: `sh build.sh OUT`.
