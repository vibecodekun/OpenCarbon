// Headless export: decompiled C for every function, a function index, and string xrefs.
// Args: <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.decompiler.parallel.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.util.task.TaskMonitor;
import java.io.*;
import java.util.*;

public class ExportAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "C:/opencarbon/ghidra/export";
        new File(out).mkdirs();
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        List<Function> funcs = new ArrayList<>();
        for (Function f : fm.getFunctions(true)) if (!f.isThunk() && !f.isExternal()) funcs.add(f);

        try (PrintWriter idx = new PrintWriter(new FileWriter(out + "/functions.tsv"))) {
            idx.println("addr\tname\tsize\tcallers\tcallees");
            for (Function f : fm.getFunctions(true)) {
                idx.printf("%s\t%s\t%d\t%d\t%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses(),
                        f.getCallingFunctions(monitor).size(), f.getCalledFunctions(monitor).size());
            }
        }

        try (PrintWriter sx = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out + "/strings_xrefs.tsv"), "UTF-8"))) {
            DataIterator it = listing.getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                if (!(d.getValue() instanceof String)) continue;
                StringBuilder refs = new StringBuilder();
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    refs.append(f != null ? f.getName() : r.getFromAddress().toString()).append(',');
                }
                String s = ((String) d.getValue()).replace("\n", "\n").replace("\t", "\t");
                sx.printf("%s\t%s\t%s%n", d.getAddress(), refs, s);
            }
        }

        DecompilerCallback<String> cb = new DecompilerCallback<>(currentProgram, new DecompileConfigurer() {
            public void configure(DecompInterface d) {
                DecompileOptions o = new DecompileOptions();
                o.grabFromProgram(currentProgram);
                d.setOptions(o);
                d.toggleCCode(true);
                d.setSimplificationStyle("decompile");
            }
        }) {
            public String process(DecompileResults r, TaskMonitor m) {
                Function f = r.getFunction();
                String hdr = "// ==== " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + "\n";
                if (r.getDecompiledFunction() == null) return hdr + "// decompile failed: " + r.getErrorMessage() + "\n";
                return hdr + r.getDecompiledFunction().getC();
            }
        };
        cb.setTimeout(120);
        List<String> results = ParallelDecompiler.decompileFunctions(cb, funcs, monitor);
        cb.dispose();
        Map<String, String> byAddr = new TreeMap<>();
        for (String s : results) {
            int at = s.indexOf(" @ ");
            byAddr.put(s.substring(at + 3, s.indexOf(' ', at + 3)), s);
        }
        try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out + "/all.c"), "UTF-8"))) {
            for (String s : byAddr.values()) { w.println(s); }
        }
        println("exported " + byAddr.size() + " functions to " + out);
    }
}
