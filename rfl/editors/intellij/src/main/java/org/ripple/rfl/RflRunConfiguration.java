package org.ripple.rfl;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.Executor;
import com.intellij.execution.configurations.*;
import com.intellij.execution.executors.DefaultRunExecutor;
import com.intellij.execution.filters.RegexpFilter;
import com.intellij.execution.process.*;
import com.intellij.execution.runners.ExecutionEnvironment;
import com.intellij.openapi.components.PathMacroManager;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.util.EnvironmentUtil;
import org.jdom.Element;
import org.jetbrains.annotations.NotNull;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;

public final class RflRunConfiguration extends LocatableConfigurationBase<LocatableRunConfigurationOptions> {
    private RflCommand.Options commandOptions = new RflCommand.Options();

    RflRunConfiguration(Project project, ConfigurationFactory factory, String name) { super(project, factory, name); }
    RflCommand.Options commandOptions() { return commandOptions; }
    void setCommandOptions(RflCommand.Options options) { commandOptions = options.copy(); }

    boolean matchesSource(String path) {
        try {
            String value = PathMacroManager.getInstance(getProject()).expandPath(commandOptions.source);
            Path source = Path.of(value);
            if (!source.isAbsolute()) {
                if (projectDirectory() == null) return false;
                source = projectDirectory().resolve(source);
            }
            return source.toAbsolutePath().normalize().equals(Path.of(path).toAbsolutePath().normalize());
        } catch (java.nio.file.InvalidPathException e) {
            return false;
        }
    }

    @Override public @NotNull SettingsEditor<RflRunConfiguration> getConfigurationEditor() {
        return new RflRunSettingsEditor(getProject());
    }

    @Override public String suggestedName() {
        if (commandOptions.source.isBlank()) return "RFL";
        String file = commandOptions.source.replace('\\', '/');
        file = file.substring(file.lastIndexOf('/') + 1);
        return switch (commandOptions.mode) {
            case "compile" -> "Compile " + file;
            case "check" -> "Check " + file;
            default -> file;
        };
    }

    @Override public boolean excludeCompileBeforeLaunchOption() { return true; }

    @Override public void readExternal(@NotNull Element element) {
        super.readExternal(element);
        commandOptions = RflCommand.Options.read(element);
    }

    @Override public void writeExternal(@NotNull Element element) {
        super.writeExternal(element);
        commandOptions.write(element);
    }

    @Override public RflRunConfiguration clone() {
        RflRunConfiguration copy = (RflRunConfiguration) super.clone();
        copy.commandOptions = commandOptions.copy();
        return copy;
    }

    @Override public void checkConfiguration() throws RuntimeConfigurationException {
        try {
            RflCommand.plan(expandedOptions(), projectDirectory());
            executable();
        } catch (IOException e) {
            throw new RuntimeConfigurationError(e.getMessage());
        }
    }

    @Override public RunProfileState getState(@NotNull Executor executor, @NotNull ExecutionEnvironment environment) throws ExecutionException {
        if (!DefaultRunExecutor.EXECUTOR_ID.equals(executor.getId())) {
            throw new ExecutionException("RFL configurations support Run. Debugging is not implemented.");
        }
        return new CommandLineState(environment) {
            @Override protected @NotNull ProcessHandler startProcess() throws ExecutionException {
                try {
                    RflCommand.Plan plan = RflCommand.plan(expandedOptions(), projectDirectory());
                    GeneralCommandLine command = new GeneralCommandLine(executable().toString())
                        .withParameters(plan.arguments())
                        .withWorkDirectory(plan.workingDirectory().toFile())
                        .withCharset(StandardCharsets.UTF_8)
                        .withParentEnvironmentType(GeneralCommandLine.ParentEnvironmentType.CONSOLE);
                    if (plan.terminal()) command = new PtyCommandLine(command).withConsoleMode(false);
                    KillableColoredProcessHandler handler = new KillableColoredProcessHandler(command);
                    handler.setShouldDestroyProcessRecursively(true);
                    ProcessTerminatedListener.attach(handler);
                    return handler;
                } catch (IOException e) {
                    throw new ExecutionException(e.getMessage(), e);
                }
            }
            {
                getConsoleBuilder().addFilter(new RegexpFilter(getProject(), "\\s*--> $FILE_PATH$:$LINE$:$COLUMN$.*"));
            }
        };
    }

    private Path projectDirectory() {
        return getProject().getBasePath() == null ? null : Path.of(getProject().getBasePath());
    }

    private Path executable() throws IOException {
        return ServerExecutable.resolve(RflSettings.getInstance(getProject()).getServerPath(), projectDirectory(), EnvironmentUtil.getValue("PATH"));
    }

    private RflCommand.Options expandedOptions() {
        RflCommand.Options options = commandOptions.copy();
        PathMacroManager macros = PathMacroManager.getInstance(getProject());
        options.source = macros.expandPath(options.source);
        options.workingDirectory = macros.expandPath(options.workingDirectory);
        options.output = macros.expandPath(options.output);
        return options;
    }
}
