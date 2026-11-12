import lldb, struct
out = open("/tmp/lsd/cap.bin","wb")
n = [0]
def cb(frame, bp_loc, d):
    t = frame.GetThread(); p = t.GetProcess()
    rdi = frame.FindRegister("rdi").GetValueAsUnsigned()
    rdx = frame.FindRegister("rdx").GetValueAsUnsigned()
    r8 = frame.FindRegister("r8").GetValueAsUnsigned()
    mid = r8 >> 32
    size = rdx >> 32
    if 0x9c42 <= mid <= 0x9c45 or mid in (0x9d0a,):
        err = lldb.SBError()
        data = p.ReadMemory(rdi, size, err)
        if err.Success():
            out.write(struct.pack("<I", size) + data); out.flush(); n[0]+=1
    return False
def __lldb_init_module(dbg, d):
    tgt = dbg.GetSelectedTarget()
    bp = tgt.BreakpointCreateByName("mach_msg2_trap")
    bp.SetScriptCallbackFunction("cap.cb")
    print("bp locations", bp.GetNumLocations())

import time
def run(dbg=None):
    d = lldb.debugger
    d.SetAsync(False)
    p = d.GetSelectedTarget().GetProcess()
    t0 = time.time()
    while time.time() - t0 < 75 and p.GetState() == lldb.eStateStopped:
        for th in p.threads:
            if th.GetStopReason() == lldb.eStopReasonException:
                print("exception stop:", th.GetStopDescription(200))
        p.Continue()
    print("done, state", p.GetState(), "captured", n[0])
    p.Detach()
