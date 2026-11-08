# runs the wrapped Keka in the desktop session of the pearOS image (the window server socket and the session variables)
export MACHRUN_COMPAT=/var/tmp/libmachcompat.so MALLOC_CONF=junk:true MACHRUN_OPENFILE=/var/tmp/arc/demo.zip MACHRUN_SYMBOLS=/var/tmp/keka.syms MACHRUN_SHOWWINDOWS=1 MACHRUN_LENIENT=1 WS_SOCKET=/var/run/WindowServer.sock HOME=/Users/alex XDG_SESSION_TYPE=wsd XDG_CURRENT_DESKTOP=Aqua XDG_SESSION_DESKTOP=Aqua LANG=C.UTF-8
ulimit -c 0
cd /var/tmp/Keka.app
LD_PRELOAD=/var/tmp/libexctrace.so timeout -s KILL ${1:-40} ./Keka 2>&1 | cut -c1-210
