for p in $(ps ax -o pid,command | grep -a "/System/\|/root/dnew\|carender\|wsd\|cfprefsd\|Stickies\|root/win" | grep -v "grep\|killall" | awk '{print $1}'); do kill -9 $p; done
