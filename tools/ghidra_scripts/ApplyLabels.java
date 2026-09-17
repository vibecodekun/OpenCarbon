// Apply labels from TSV files: addr \t name \t F|D \t comment. F = ensure function exists.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;

public class ApplyLabels extends GhidraScript {
    @Override
    public void run() throws Exception {
        int nf = 0, nl = 0;
        for (String path : getScriptArgs()) {
            for (String line : Files.readAllLines(Paths.get(path))) {
                if (line.isBlank() || line.startsWith("#")) continue;
                String[] c = line.split("\t");
                Address a = toAddr(c[0].startsWith("0x") ? c[0].substring(2) : c[0]);
                String name = c[1];
                boolean func = c.length > 2 && c[2].equals("F");
                if (func) {
                    if (getInstructionAt(a) == null) {
                        new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                    }
                    Function f = getFunctionAt(a);
                    if (f == null) {
                        Function inside = getFunctionContaining(a);
                        if (inside != null && !inside.getEntryPoint().equals(a)) {
                            // split: shrink the containing function by recreating it after this entry exists
                            removeFunction(inside);
                            new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
                            new CreateFunctionCmd(inside.getEntryPoint()).applyTo(currentProgram, monitor);
                        } else {
                            new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
                        }
                        f = getFunctionAt(a);
                        nf++;
                    }
                    if (f != null) f.setName(name, SourceType.USER_DEFINED);
                    else println("could not create function at " + a);
                } else {
                    createLabel(a, name, true, SourceType.USER_DEFINED);
                }
                if (c.length > 3) setPlateComment(a, c[3]);
                nl++;
            }
        }
        println("applied " + nl + " labels, created " + nf + " functions");
    }
}
