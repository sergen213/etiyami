// Trace startup imports, configuration strings and their callers.
// @category GameCompatibility
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;
import java.util.LinkedHashSet;

public class TraceStartup extends GhidraScript {
    public void run() throws Exception {
        LinkedHashSet<Function> functions = new LinkedHashSet<>();
        PrintWriter out = new PrintWriter(getScriptArgs()[0]);
        String pattern = "(?i).*(game\\.ini|fullscreen|resolution|wglCreateContext|ChangeDisplaySettings|AVIStreamGetFrameOpen|AVIStreamOpenFromFile|CreateProcess|QueryPerformance|glFrustum|glViewport|FSOUND_Init|DirectInput8Create).*";
        for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
            if (!s.getName().matches(pattern)) continue;
            out.println("SYMBOL " + s.getAddress() + " " + s.getName());
            for (Reference r : getReferencesTo(s.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("  XREF " + r.getFromAddress() + " " + (f == null ? "data" : f.getName()));
                if (f != null) functions.add(f);
            }
        }
        DataIterator data = currentProgram.getListing().getDefinedData(true);
        while (data.hasNext()) {
            Data d = data.next();
            if (!(d.getValue() instanceof String) || !d.getValue().toString().matches(pattern)) continue;
            out.println("STRING " + d.getAddress() + " " + d.getValue());
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("  XREF " + r.getFromAddress() + " " + (f == null ? "data" : f.getName()));
                if (f != null) functions.add(f);
            }
        }
        for (String arg : getScriptArgs()) {
            if (!arg.startsWith("0x")) continue;
            Address a = toAddr(arg.substring(2));
            Function f = getFunctionContaining(a);
            if (f != null) functions.add(f);
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (Function f : functions) {
            out.println("\nFUNCTION " + f.getEntryPoint() + " " + f.getName());
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function caller = getFunctionContaining(r.getFromAddress());
                out.println("CALLER " + r.getFromAddress() + " " + (caller == null ? "data" : caller.getName()));
            }
            var result = dec.decompileFunction(f, 60, monitor);
            out.println(result.decompileCompleted() ? result.getDecompiledFunction().getC() : result.getErrorMessage());
        }
        dec.dispose();
        out.close();
    }
}
