// Export decompiler evidence, disassembly, strings, and a call graph.
// @category GameReconstruction
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import java.io.PrintWriter;

public class RecoverEngine extends GhidraScript {
    public void run() throws Exception {
        Path dir = Path.of(getScriptArgs()[0]);
        Files.createDirectories(dir.resolve("functions"));
        String[][] names = {
            {"00417390", "game_initialize"}, {"00417280", "game_frame"},
            {"00415d00", "renderer_initialize"}, {"004328a0", "wgl_create_game_context"},
            {"004327b0", "display_create_fullscreen"}, {"00433080", "win32_create_game_window"},
            {"0043a7e0", "renderer_set_fixed_viewport"}, {"0043b1b0", "renderer_set_orthographic"},
            {"0043b230", "renderer_set_camera_projection"}, {"00432d90", "display_change_mode"},
            {"0041bf70", "settings_read"}, {"0041ca60", "settings_write"},
            {"00411240", "directinput_create_devices"}, {"004113b0", "input_initialize"},
            {"0042c330", "audio_initialize"}, {"004087d0", "avi_open_video_texture"},
            {"004163b0", "game_render_frame"}, {"00430200", "clock_initialize"},
            {"00420260", "menu_initialize"}
        };
        for (String[] pair : names) {
            Function f = getFunctionAt(toAddr(pair[0]));
            if (f != null) f.setName(pair[1], SourceType.USER_DEFINED);
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        int total = 0, recovered = 0, failed = 0;
        try (PrintWriter index = new PrintWriter(dir.resolve("functions.tsv").toFile());
             PrintWriter graph = new PrintWriter(dir.resolve("calls.tsv").toFile());
             PrintWriter asm = new PrintWriter(dir.resolve("disassembly.txt").toFile());
             PrintWriter errors = new PrintWriter(dir.resolve("decompiler-errors.txt").toFile())) {
            index.println("address\tname\tsize\tthunk\tstatus");
            graph.println("caller\tcallsite\ttarget\ttarget_name");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                monitor.checkCancelled();
                total++;
                String address = f.getEntryPoint().toString();
                var result = dec.decompileFunction(f, 30, monitor);
                boolean ok = result.decompileCompleted();
                index.println(address + "\t" + f.getName() + "\t" + f.getBody().getNumAddresses() + "\t" + f.isThunk() + "\t" + (ok ? "decompiled" : "failed"));
                if (ok) {
                    recovered++;
                    Files.writeString(dir.resolve("functions").resolve(address + ".c"),
                        "/* Decompiled evidence; not reconstructed compilable source. VA: " + address + " */\n" + result.getDecompiledFunction().getC());
                } else {
                    failed++;
                    errors.println(address + " " + f.getName() + ": " + result.getErrorMessage());
                }
                asm.println("\nFUNCTION " + address + " " + f.getName());
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    StringBuilder bytes = new StringBuilder();
                    for (byte b : ins.getBytes()) bytes.append(String.format("%02x", b & 255));
                    asm.println(ins.getAddress() + " " + bytes + " " + ins);
                    for (Reference r : ins.getReferencesFrom()) {
                        if (!r.getReferenceType().isCall()) continue;
                        Function target = getFunctionAt(r.getToAddress());
                        graph.println(address + "\t" + ins.getAddress() + "\t" + r.getToAddress() + "\t" + (target == null ? "indirect_or_external" : target.getName()));
                    }
                }
            }
        }
        try (PrintWriter out = new PrintWriter(dir.resolve("strings.tsv").toFile())) {
            out.println("address\tvalue\txrefs");
            for (Data d : currentProgram.getListing().getDefinedData(true)) {
                if (!(d.getValue() instanceof String)) continue;
                StringBuilder refs = new StringBuilder();
                for (Reference r : getReferencesTo(d.getAddress())) refs.append(r.getFromAddress()).append(",");
                out.println(d.getAddress() + "\t" + d.getValue().toString().replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t") + "\t" + refs);
            }
        }
        dec.dispose();
        Files.writeString(dir.resolve("status.txt"), "Functions: " + total + "\nDecompiled: " + recovered + "\nFailed: " + failed + "\nThese are decompiler artifacts, not a native engine or original source.\n");
        println("Recovered " + recovered + " / " + total + " functions; failed " + failed);
    }
}
