# lldb script: log libSystem mmap/munmap/mprotect/madvise calls (args, caller,
# result) up to the first fork(), then dump vmmap and objc's TLS slot there.
# usage: lldb --batch -o "command script import trace_vm.py" -o "vmtrace OUT" -- ape clang ...
import lldb, os

OUT = None
LOG = None


def caller(frame):
    t = frame.GetThread()
    f1 = t.GetFrameAtIndex(1)
    m = f1.GetModule().GetFileSpec().GetFilename() or "?"
    return "%s:0x%x" % (m, f1.GetPC())


def regs(frame, n):
    return [frame.FindRegister("x%d" % i).GetValueAsUnsigned() for i in range(n)]


def on_call(frame, bp_loc, extra, internal_dict):
    name = frame.GetFunctionName()
    a = regs(frame, 6)
    t = frame.GetThread()
    # step out to read the return value
    lr = frame.FindRegister("lr").GetValueAsUnsigned()
    n = {"mmap": 6, "munmap": 2, "mprotect": 3, "madvise": 3}.get(name, 3)
    LOG.write("%s(%s) from %s" % (name, ", ".join("0x%x" % x for x in a[:n]), caller(frame)))
    LOG.write("\n")
    if name == "mmap":
        tg = t.GetProcess().GetTarget()
        rb = tg.BreakpointCreateByAddress(lr)
        rb.SetOneShot(True)
        rb.SetThreadID(t.GetThreadID())
        rb.SetScriptCallbackFunction("trace_vm.on_ret")
    LOG.flush()
    return False


def on_ret(frame, bp_loc, extra, internal_dict):
    LOG.write("  -> 0x%x\n" % frame.FindRegister("x0").GetValueAsUnsigned())
    LOG.flush()
    return False


def on_fork(frame, bp_loc, extra, internal_dict):
    p = frame.GetThread().GetProcess()
    LOG.write("fork() from %s\n" % caller(frame))
    LOG.flush()
    os.system("vmmap -w %d > %s.vmmap 2>&1" % (p.GetProcessID(), OUT))
    return True  # stop


def vmtrace(debugger, command, result, internal_dict):
    global OUT, LOG
    OUT = command.strip()
    LOG = open(OUT + ".calls", "w")
    t = debugger.GetSelectedTarget()
    for fn in ("mmap", "munmap", "mprotect", "madvise"):
        bp = t.BreakpointCreateByName(fn, "libsystem_kernel.dylib")
        bp.SetScriptCallbackFunction("trace_vm.on_call")
    bp = t.BreakpointCreateByName("fork", "libsystem_c.dylib")
    bp.SetScriptCallbackFunction("trace_vm.on_fork")
    debugger.SetAsync(False)
    err = lldb.SBError()
    li = t.GetLaunchInfo()
    p = t.Launch(li, err)
    if not err.Success():
        print("launch failed", err)
        return
    th = p.GetSelectedThread()
    f = th.GetFrameAtIndex(0)
    for k in (40, 41, 42):
        v = f.EvaluateExpression("(void*)pthread_getspecific(%d)" % k)
        LOG.write("tsd[%d] = %s\n" % (k, v.GetValue()))
    LOG.close()
    p.Kill()


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f trace_vm.vmtrace vmtrace")
