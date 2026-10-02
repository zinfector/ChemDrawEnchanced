// Export established function boundaries/prototypes from the user's database.
// Run headless with -readOnly -noanalysis. Never modifies the program.
// @category ChemDraw
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
public class ExportPatchTargets extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        Path root=Paths.get(args[0]);
        List<String> output=new ArrayList<>();
        for(String line:Files.readAllLines(root.resolve("targets.tsv"))) {
            String[] p=line.split("\t");
            if(!p[0].equals(currentProgram.getName()))continue;
            long rva=Long.parseUnsignedLong(p[1],16);
            var address=currentProgram.getImageBase().add(rva);
            Function f=currentProgram.getFunctionManager().getFunctionAt(address);
            if(f==null) { output.add(line+"\tMISSING\t\t");continue; }
            int count=(int)Math.min(192,f.getBody().getMaxAddress().subtract(address)+1);
            byte[] bytes=new byte[count];currentProgram.getMemory().getBytes(address,bytes);
            StringBuilder hex=new StringBuilder();for(byte b:bytes)hex.append(String.format("%02x",b&255));
            output.add(line+"\t"+f.getName()+"\t"+f.getPrototypeString(false,false).replace('\t',' ')+"\t"+hex);
        }
        Files.write(root.resolve(currentProgram.getName()+".targets.tsv"),output,StandardCharsets.UTF_8);
        println("Exported "+output.size()+" patch functions for "+currentProgram.getName());
    }
}
