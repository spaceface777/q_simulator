# lldb script: who calls objc_sync_enter in a cosmo program before fork(),
# and what objc's sDataLists (the @synchronized nodes the fork child frees)
# hold at the fork. usage: lldb --batch -o "command script import objcsync.py"
#   -o "objcsync OUT" -- ape clang ...
import lldb

LOG = None
hits = 0


def on_sync(frame, bp_loc, extra, internal_dict):
    global hits
    hits += 1
    if hits <= 5:
        LOG.write("objc_sync_enter #%d obj=0x%x\n" % (hits, frame.FindRegister("x0").GetValueAsUnsigned()))
        for f in frame.GetThread().frames[:16]:
            LOG.write("   %s\n" % f)
    return False


def on_fork(frame, bp_loc, extra, internal_dict):
    t = frame.GetThread().GetProcess().GetTarget()
    p = t.GetProcess()
    LOG.write("fork: objc_sync_enter calls so far: %d\n" % hits)
    syms = t.FindSymbols("sDataLists")
    for sc in syms:
        a = sc.GetSymbol().GetStartAddress().GetLoadAddress(t)
        LOG.write("sDataLists at 0x%x\n" % a)
        err = lldb.SBError()
        for i in range(64):
            head = p.ReadPointerFromMemory(a + 64 * i, err)
            n = 0
            while head and n < 8:
                reg = lldb.SBMemoryRegionInfo()
                p.GetMemoryRegionInfo(head, reg)
                LOG.write("  stripe %d node 0x%x region [0x%x-0x%x) %s%s%s\n" % (
                    i, head, reg.GetRegionBase(), reg.GetRegionEnd(),
                    "r" if reg.IsReadable() else "-", "w" if reg.IsWritable() else "-",
                    "x" if reg.IsExecutable() else "-"))
                head = p.ReadPointerFromMemory(head, err)
                n += 1
    LOG.flush()
    return True


def objcsync(debugger, command, result, internal_dict):
    global LOG
    LOG = open(command.strip(), "w")
    t = debugger.GetSelectedTarget()
    bp = t.BreakpointCreateByRegex("objc_sync_enter|id2data|objc_sync_try_enter", "libobjc.A.dylib")
    bp.SetScriptCallbackFunction("objcsync.on_sync")
    bp = t.BreakpointCreateByName("fork", "libsystem_c.dylib")
    bp.SetScriptCallbackFunction("objcsync.on_fork")
    debugger.SetAsync(False)
    err = lldb.SBError()
    p = t.Launch(t.GetLaunchInfo(), err)
    if not err.Success():
        LOG.write("launch failed: %s\n" % err)
    LOG.write("stopped: %s\n" % p.GetSelectedThread().GetStopDescription(200))
    LOG.close()
    p.Kill()


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f objcsync.objcsync objcsync")
