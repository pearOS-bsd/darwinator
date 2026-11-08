fetch -q -o /tmp/machrun http://10.0.2.2:8000/machrun; chmod +x /tmp/machrun
fetch -q -o /tmp/wrapapp.sh http://10.0.2.2:8000/wrapapp.sh
sh /tmp/wrapapp.sh /mnt/Keka.app /tmp /tmp/machrun >/dev/null
# the nibs, with the fix of tools/nibfix.py (done on the host: the image has no python)
for n in about advanced advancedWindowPopover logPopoverView loveView loveViewWEB main preferences taskLegacy task; do
	for L in en.lproj English.lproj Base.lproj; do
		D=/tmp/Keka.app/Resources/$L; [ -e $D/$n.nib ] || [ -L $D/$n.nib ] || continue
		rm -rf $D/$n.nib; mkdir -p $D/$n.nib; fetch -q -o $D/$n.nib/keyedobjects.nib http://10.0.2.2:8000/nibs/$n.nib/keyedobjects.nib
	done
done
pkill -x Keka 2>/dev/null
cd /tmp/Keka.app; (./Keka > /tmp/k.log 2>&1 &); sleep ${1:-25}; fold -w 76 /tmp/k.log | tail -${2:-14}
