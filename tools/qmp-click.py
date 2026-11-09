#!/usr/bin/env python3
# qmp-click.py click|double|move X Y: mouse input for a QEMU guest with a usb-tablet, through QMP (start QEMU with -qmp unix:qmp,server,nowait next to this script; X Y are pixels of a 1920x1080 screen).
# The monitor command mouse_move does not move the absolute tablet; input-send-event does.
import socket, json, sys, time, os
s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'qmp')); f = s.makefile('rw')
def cmd(c, a=None):
    f.write(json.dumps({'execute': c, **({'arguments': a} if a else {})}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
f.readline(); cmd('qmp_capabilities')
op = sys.argv[1]; x = int(int(sys.argv[2]) / 1920 * 32767); y = int(int(sys.argv[3]) / 1080 * 32767)
def move(): print(cmd('input-send-event', {'events': [{'type': 'abs', 'data': {'axis': 'x', 'value': x}}, {'type': 'abs', 'data': {'axis': 'y', 'value': y}}]}))
def click():
    cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]}); time.sleep(0.08)
    cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]}); time.sleep(0.08)
move(); time.sleep(0.4)
if op == 'click': click()
if op == 'double': click(); click()
