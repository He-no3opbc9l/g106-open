// Dump decompiled C of every function into one file, sorted by address.
// Arg: output path.
//@category G106
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        ifc.setOptions(opts);
        ifc.openProgram(currentProgram);
        int ok = 0, fail = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled())
                    break;
                DecompileResults r = ifc.decompileFunction(f, 60, monitor);
                w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
                if (r != null && r.decompileCompleted()) {
                    w.println(r.getDecompiledFunction().getC());
                    ok++;
                } else {
                    w.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
                    fail++;
                }
            }
        }
        ifc.dispose();
        println("exported " + ok + " functions, " + fail + " failed -> " + out);
    }
}
