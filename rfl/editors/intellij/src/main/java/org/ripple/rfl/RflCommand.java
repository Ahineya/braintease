package org.ripple.rfl;

import org.jdom.Element;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/** Validates and builds CLI arguments without invoking a shell. */
final class RflCommand {
    enum Mode {
        RUN("run", "Run"), COMPILE("compile", "Compile"), CHECK("check", "Check");
        final String argument;
        private final String label;
        Mode(String argument, String label) { this.argument = argument; this.label = label; }
        @Override public String toString() { return label; }
    }

    static final class Options {
        String source = "";
        String workingDirectory = "";
        String output = "";
        String mode = "run";
        String target = "ripple";
        boolean jit;
        boolean emitIr;
        boolean terminal = true;

        Options copy() {
            Options copy = new Options();
            copy.source = source;
            copy.workingDirectory = workingDirectory;
            copy.output = output;
            copy.mode = mode;
            copy.target = target;
            copy.jit = jit;
            copy.emitIr = emitIr;
            copy.terminal = terminal;
            return copy;
        }

        void write(Element parent) {
            parent.addContent(new Element("rfl")
                .setAttribute("source", source).setAttribute("workingDirectory", workingDirectory)
                .setAttribute("output", output).setAttribute("mode", mode).setAttribute("target", target)
                .setAttribute("jit", Boolean.toString(jit)).setAttribute("emitIr", Boolean.toString(emitIr))
                .setAttribute("terminal", Boolean.toString(terminal)));
        }

        static Options read(Element parent) {
            Options options = new Options();
            Element state = parent.getChild("rfl");
            if (state != null) {
                options.source = state.getAttributeValue("source", "");
                options.workingDirectory = state.getAttributeValue("workingDirectory", "");
                options.output = state.getAttributeValue("output", "");
                options.mode = state.getAttributeValue("mode", "run");
                options.target = state.getAttributeValue("target", "ripple");
                options.jit = Boolean.parseBoolean(state.getAttributeValue("jit", "false"));
                options.emitIr = Boolean.parseBoolean(state.getAttributeValue("emitIr", "false"));
                options.terminal = Boolean.parseBoolean(state.getAttributeValue("terminal", "true"));
            }
            return options;
        }
    }

    record Plan(List<String> arguments, Path workingDirectory, boolean terminal) {}

    static Plan plan(Options options, Path projectDirectory) throws IOException {
        try {
            if (options.source.isBlank()) throw new IOException("Select an RFL source file.");
            boolean run = options.mode.equals("run"), compile = options.mode.equals("compile");
            if (!run && !compile && !options.mode.equals("check")) throw new IOException("Choose Run, Compile or Check.");
            if (!options.target.equals("ripple") && !options.target.equals("native")) throw new IOException("Choose the Ripple or native target.");
            if (options.jit && (!run || !options.target.equals("ripple"))) throw new IOException("JIT is available only when running the Ripple target.");
            if (options.emitIr && !compile) throw new IOException("Emit IR is available only for Compile.");
            if (!options.output.isBlank() && (!compile || options.emitIr)) throw new IOException("An output file is available only for Compile without Emit IR.");

            Path source = resolve(options.source, projectDirectory);
            if (!Files.isRegularFile(source) || !source.getFileName().toString().endsWith(".rfl")) {
                throw new IOException("RFL source file does not exist or is not an .rfl file: " + source);
            }
            Path working = options.workingDirectory.isBlank()
                ? (projectDirectory == null ? source.getParent() : projectDirectory)
                : resolve(options.workingDirectory, projectDirectory);
            working = working.toAbsolutePath().normalize();
            if (!Files.isDirectory(working)) throw new IOException("Working directory does not exist: " + working);

            List<String> arguments = new ArrayList<>(List.of(options.mode, source.toString(), "--target", options.target));
            if (options.jit) arguments.add("--jit");
            if (options.emitIr) arguments.add("--emit-ir");
            if (!options.output.isBlank()) {
                Path output = resolve(options.output, working);
                if (output.equals(source) || Files.exists(output) && Files.isSameFile(output, source)) {
                    throw new IOException("The output file must differ from the source file.");
                }
                if (Files.isDirectory(output) || !Files.isDirectory(output.getParent())) {
                    throw new IOException("Choose an output file in an existing directory: " + output);
                }
                arguments.add("--output");
                arguments.add(output.toString());
            }
            return new Plan(List.copyOf(arguments), working, run && options.terminal);
        } catch (InvalidPathException e) {
            throw new IOException("Invalid path: " + e.getInput(), e);
        }
    }

    private static Path resolve(String value, Path base) throws IOException {
        String text = value.strip();
        if (text.startsWith("~/") || text.startsWith("~\\")) text = System.getProperty("user.home") + text.substring(1);
        Path path = Path.of(text);
        if (!path.isAbsolute()) {
            if (base == null) throw new IOException("Use absolute paths when no project directory is available.");
            path = base.resolve(path);
        }
        return path.toAbsolutePath().normalize();
    }

    private RflCommand() {}
}
