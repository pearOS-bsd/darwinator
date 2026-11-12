# the stack that starts the Dock of the Mac on the VM (what each service is: see lsd-stub/README.md, svc-stub/README.md); run on the FreeBSD VM as root after kldload; PRE=seconds before the program
for p in svc-stub xpc-stub ControlCenter cfprefsd lsd-stub carender-stub; do pkill -9 $p; done
pkill -9 -f macapps; pkill -9 -f macbins; pkill -9 -f "lsd-st"; pkill -9 -f "carender-st"; pkill -9 wsbridge; pkill -9 notifyd; pkill -9 launchd; sleep 1
sysctl kern.darwinator.dyld=1 kern.darwinator.debug=1 >/dev/null; dmesg -c >/dev/null
(cd /tmp; nohup /root/dnew/launchd/launchd /root/dnew/notifyd/notifyd /root/dnew/wsbridge/wsbridge "/root/macbins/usr/sbin/cfprefsd daemon" "/root/macbins/usr/sbin/cfprefsd agent" /root/dnew/lsd-stub/out/lsd-stub /root/dnew/carender-stub/out/carender-stub "/root/dnew/svc-stub/out/svc-stub com.apple.PowerManagement.control" "/root/dnew/lsd-stub/out/xpc-stub com.apple.fonts com.apple.backlightd com.apple.pasteboard.1 com.apple.tccd.system com.apple.distributed_notifications@1v3 com.apple.lsd.modifydb com.apple.logd" "/root/dnew/svc-stub/out/svc-stub com.apple.CoreServices.coreservicesd" "/root/dnew/lsd-stub/out/xpc-stub com.apple.windowmanager.external" "/root/dnew/lsd-stub/out/xpc-stub com.apple.coreservices.appleevents com.apple.wallpaper com.apple.uninstall.service com.apple.carboncore.csnameddata com.apple.dmd.policy com.apple.usernoted.dock" >/tmp/launchd.log 2>&1 &)
sleep ${PRE:-6}
(cd /tmp; nohup env OS_ACTIVITY_DT_MODE=YES OBJC_PRINT_EXCEPTION_THROW=YES /root/macapps/Dock </dev/null >/tmp/Dock.out 2>&1 &)
sleep ${WAIT:-15}
