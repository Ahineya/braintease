package org.ripple.rfl;

import com.intellij.execution.ProgramRunnerUtil;
import com.intellij.execution.RunManager;
import com.intellij.execution.RunnerAndConfigurationSettings;
import com.intellij.execution.executors.DefaultRunExecutor;
import com.intellij.openapi.actionSystem.*;
import com.intellij.openapi.project.DumbAwareAction;
import com.intellij.openapi.vfs.VirtualFile;
import org.jetbrains.annotations.NotNull;

/** Compile/check shortcuts use normal reusable IDEA run configurations. */
public abstract class RflFileAction extends DumbAwareAction {
    private final String mode;

    protected RflFileAction(String mode) { this.mode = mode; }

    @Override public @NotNull ActionUpdateThread getActionUpdateThread() { return ActionUpdateThread.BGT; }

    @Override public void update(@NotNull AnActionEvent event) {
        event.getPresentation().setEnabledAndVisible(event.getProject() != null && sourceFile(event) != null);
    }

    @Override public void actionPerformed(@NotNull AnActionEvent event) {
        VirtualFile file = sourceFile(event);
        if (event.getProject() == null || file == null) return;
        RunManager manager = RunManager.getInstance(event.getProject());
        RunnerAndConfigurationSettings settings = manager.getAllSettings().stream()
            .filter(item -> item.getConfiguration() instanceof RflRunConfiguration configuration
                && configuration.commandOptions().mode.equals(mode) && configuration.matchesSource(file.getPath()))
            .findFirst().orElse(null);
        if (settings == null) {
            settings = manager.createConfiguration(file.getName(), RflRunConfigurationType.factory());
            RflRunConfiguration configuration = (RflRunConfiguration) settings.getConfiguration();
            RflCommand.Options options = configuration.commandOptions().copy();
            options.source = file.getPath();
            options.mode = mode;
            options.jit = false;
            options.emitIr = false;
            options.output = "";
            configuration.setCommandOptions(options);
            configuration.setGeneratedName();
            manager.setTemporaryConfiguration(settings);
        }
        manager.setSelectedConfiguration(settings);
        ProgramRunnerUtil.executeConfiguration(settings, DefaultRunExecutor.getRunExecutorInstance());
    }

    private static VirtualFile sourceFile(AnActionEvent event) {
        VirtualFile file = event.getData(CommonDataKeys.VIRTUAL_FILE);
        return file != null && !file.isDirectory() && file.isInLocalFileSystem() && "rfl".equals(file.getExtension()) ? file : null;
    }

    public static final class Compile extends RflFileAction {
        public Compile() { super("compile"); }
    }

    public static final class Check extends RflFileAction {
        public Check() { super("check"); }
    }
}
