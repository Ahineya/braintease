package org.ripple.rfl;

import org.jdom.Element;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Tests argument generation, persisted state and the real compiler when supplied. */
public final class RunConfigurationTest {
    public static void main(String[] args) throws Exception {
        entryPoints();
        Path build = Path.of(args.length == 0 ? "build" : args[0]).toAbsolutePath();
        Files.createDirectories(build);
        Path temporary = Files.createTempDirectory(build, "run checks ");
        try {
            commands(temporary);
            if (args.length > 1) compiler(temporary, Path.of(args[1]).toAbsolutePath());
        } finally {
            try (var files = Files.walk(temporary)) {
                for (Path path : files.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
            }
        }
        System.out.println("Run checks passed: entry points, CLI arguments, validation, saved settings and copy isolation.");
    }

    private static void entryPoints() {
        String text = "// fn main() {}\nlet s = \"fn main() {}\";\nimpl X { fn main() {} }\nfn /* comment */ main () {}";
        require(RflEntryPoint.offset(text) == text.lastIndexOf("main"), "only top-level main, with comments");
        for (String library : List.of("fn not_main() {}", "// fn main() {}", "let s = c\"fn main() {}\";",
                                     "struct Main { main: i16 }", "impl X { fn main() {} }", "fn main_value() {}")) {
            require(RflEntryPoint.offset(library) == -1, "no false run marker: " + library);
        }
    }

    private static void commands(Path root) throws Exception {
        Path source = root.resolve("hello world.rfl");
        Files.writeString(source, "fn main() -> i16 { 0 }");
        RflCommand.Options options = new RflCommand.Options();
        options.source = source.getFileName().toString();
        RflCommand.Plan plan = RflCommand.plan(options, root);
        require(plan.arguments().equals(List.of("run", source.toString(), "--target", "ripple")), "run with spaced path as a single argument");
        require(plan.workingDirectory().equals(root) && plan.terminal(), "project workdir and terminal default");
        options.jit = true;
        require(RflCommand.plan(options, root).arguments().contains("--jit"), "Ripple JIT");
        options.target = "native";
        invalid(options, root, "JIT");
        options.jit = false;
        require(RflCommand.plan(options, root).arguments().contains("native"), "native run");
        options.mode = "compile";
        options.output = "output with spaces.native";
        plan = RflCommand.plan(options, root);
        require(plan.arguments().getLast().equals(root.resolve(options.output).toString()), "compile output path");
        require(!plan.terminal(), "compiler does not use a terminal");
        options.emitIr = true;
        invalid(options, root, "output file");
        options.output = "";
        require(RflCommand.plan(options, root).arguments().contains("--emit-ir"), "IR on stdout");
        options.emitIr = false;
        options.output = source.toString();
        invalid(options, root, "differ");
        options.output = "missing/output.asm";
        invalid(options, root, "existing directory");
        options.output = "";
        options.mode = "check";
        require(RflCommand.plan(options, root).arguments().equals(List.of("check", source.toString(), "--target", "native")), "check target");
        options.workingDirectory = "missing";
        invalid(options, root, "Working directory");
        Path work = Files.createDirectory(root.resolve("work"));
        options.workingDirectory = "work";
        require(RflCommand.plan(options, root).workingDirectory().equals(work), "source is project-relative when workdir differs");
        options.source = "does-not-exist.rfl";
        invalid(options, root, "source file");
        options.source = source.toString();
        options.mode = "invalid";
        invalid(options, root, "Run, Compile or Check");
        options.mode = "run";
        options.target = "unknown";
        invalid(options, root, "target");

        options.target = "ripple";
        options.jit = true;
        options.terminal = false;
        Element element = new Element("configuration");
        options.write(element);
        RflCommand.Options restored = RflCommand.Options.read(element);
        require(RflCommand.plan(options, root).equals(RflCommand.plan(restored, root)), "persistence round trip");
        restored.source = "modified.rfl";
        require(options.source.equals(source.toString()), "state copy isolation");
        RflCommand.Options copy = options.copy();
        copy.jit = false;
        require(options.jit, "configuration clone isolation");
        require(RflCommand.Options.read(new Element("configuration")).terminal, "missing state gets defaults");
    }

    private static void compiler(Path root, Path binary) throws Exception {
        Path source = root.resolve("hello world.rfl");
        Files.writeString(source, "fn main() -> i16 { println(42); 0 }\n");
        RflCommand.Options options = new RflCommand.Options();
        options.source = source.toString();
        options.mode = "check";
        invoke(binary, RflCommand.plan(options, root), root, 0);
        options.mode = "compile";
        options.output = root.resolve("hello world.asm").toString();
        invoke(binary, RflCommand.plan(options, root), root, 0);
        require(Files.size(Path.of(options.output)) > 0, "Ripple assembly artifact");
        options.output = "";
        options.emitIr = true;
        require(!invoke(binary, RflCommand.plan(options, root), root, 0).isBlank(), "IR output");
        options.emitIr = false;
        options.target = "native";
        options.output = root.resolve("hello world.native").toString();
        invoke(binary, RflCommand.plan(options, root), root, 0);
        String output = invoke(Path.of(options.output), new RflCommand.Plan(List.of(), root, false), root, 0);
        require(output.strip().equals("42"), "native executable output: " + output);
        options.output = "";
        options.mode = "run";
        for (String target : List.of("ripple", "native")) {
            options.target = target;
            output = invoke(binary, RflCommand.plan(options, root), root, 0);
            require(output.contains("42"), target + " compile-and-run output: " + output);
        }
        options.target = "ripple";
        options.jit = true;
        require(invoke(binary, RflCommand.plan(options, root), root, 0).contains("42"), "JIT run output");
        options.jit = false;
        options.mode = "check";
        Files.writeString(source, "fn main() -> i16 { undefined_name }\n");
        require(invoke(binary, RflCommand.plan(options, root), root, 1).contains("error:"), "compiler error output and exit code");
        System.out.println("Compiler smoke passed: check, assembly, IR, native executable, Ripple/native run, JIT, and error exit status (including paths with spaces).");
    }

    private static String invoke(Path executable, RflCommand.Plan plan, Path root, int expected) throws Exception {
        List<String> command = new ArrayList<>();
        command.add(executable.toString());
        command.addAll(plan.arguments());
        Path log = root.resolve("process.log");
        Process process = new ProcessBuilder(command).directory(plan.workingDirectory().toFile())
            .redirectErrorStream(true).redirectOutput(log.toFile()).start();
        if (!process.waitFor(45, TimeUnit.SECONDS)) {
            process.toHandle().descendants().forEach(ProcessHandle::destroyForcibly);
            process.destroyForcibly();
            throw new AssertionError("Compiler/run timed out: " + command);
        }
        String output = Files.readString(log, StandardCharsets.UTF_8);
        require(process.exitValue() == expected, command + " exited " + process.exitValue() + ": " + output);
        return output;
    }

    private static void invalid(RflCommand.Options options, Path root, String message) throws Exception {
        try {
            RflCommand.plan(options, root);
            throw new AssertionError("Expected validation failure: " + message);
        } catch (IOException expected) {
            require(expected.getMessage().contains(message), expected.getMessage());
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
