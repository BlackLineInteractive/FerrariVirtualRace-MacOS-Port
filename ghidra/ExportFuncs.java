// Ghidra headless post-script: export functions, their bodies, and computed-jump targets as JSON lines.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "funcs.jsonl";
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                StringBuilder sb = new StringBuilder();
                sb.append("{\"type\":\"func\",\"entry\":").append(f.getEntryPoint().getOffset());
                sb.append(",\"name\":\"").append(f.getName().replace("\"", "'")).append("\"");
                sb.append(",\"thunk\":").append(f.isThunk());
                sb.append(",\"cc\":\"").append(f.getCallingConventionName()).append("\"");
                sb.append(",\"purge\":").append(f.getStackPurgeSize());
                sb.append(",\"ranges\":[");
                boolean first = true;
                for (AddressRange r : f.getBody()) {
                    if (!first) sb.append(",");
                    first = false;
                    sb.append("[").append(r.getMinAddress().getOffset()).append(",").append(r.getMaxAddress().getOffset()).append("]");
                }
                sb.append("]}");
                w.println(sb);
            }
            // computed jump / call targets (switch tables etc.)
            InstructionIterator ii = currentProgram.getListing().getInstructions(true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                FlowType ft = ins.getFlowType();
                if (ft.isComputed() && ft.isJump()) {
                    StringBuilder sb = new StringBuilder();
                    sb.append("{\"type\":\"cjump\",\"at\":").append(ins.getAddress().getOffset()).append(",\"targets\":[");
                    boolean first = true;
                    for (Reference r : ins.getReferencesFrom()) {
                        if (!r.getReferenceType().isFlow()) continue;
                        if (!first) sb.append(",");
                        first = false;
                        sb.append(r.getToAddress().getOffset());
                    }
                    sb.append("]}");
                    w.println(sb);
                }
            }
        }
        println("exported to " + out);
    }
}
