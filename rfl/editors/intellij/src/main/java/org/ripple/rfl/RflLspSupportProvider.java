package org.ripple.rfl;

import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.platform.lsp.api.*;
import com.intellij.platform.lsp.api.lsWidget.LspClientWidgetItem;
import com.intellij.util.EnvironmentUtil;
import org.jetbrains.annotations.NotNull;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;

public final class RflLspSupportProvider implements LspIntegrationProvider {
    @Override public void fileOpened(@NotNull Project project, @NotNull VirtualFile file, @NotNull LspClientStarter starter) {
        if (supported(file)) starter.ensureClientStarted(new Descriptor(project));
    }

    @Override public LspClientWidgetItem createWidgetItem(@NotNull LspClient client, VirtualFile file) {
        return new LspClientWidgetItem(client, file, RflFileType.ICON, RflConfigurable.class);
    }

    private static boolean supported(VirtualFile file) {
        return file.isInLocalFileSystem() && "rfl".equals(file.getExtension());
    }

    private static final class Descriptor extends ProjectWideLspClientDescriptor {
        private final RflLspCustomization customization = new RflLspCustomization();
        Descriptor(Project project) { super(project, "RFL"); }
        @Override public @NotNull RflLspCustomization getLspCustomization() { return customization; }
        @Override public boolean isSupportedFile(@NotNull VirtualFile file) { return supported(file); }
        @Override public @NotNull String getLanguageId(@NotNull VirtualFile file) { return "rfl"; }

        @Override public @NotNull GeneralCommandLine createCommandLine() throws com.intellij.execution.ExecutionException {
            Project project = getProject();
            Path base = project.getBasePath() == null ? null : Path.of(project.getBasePath());
            try {
                Path executable = ServerExecutable.resolve(
                    RflSettings.getInstance(project).getServerPath(), base, EnvironmentUtil.getValue("PATH"));
                GeneralCommandLine command = new GeneralCommandLine(executable.toString(), "lsp")
                    .withCharset(StandardCharsets.UTF_8)
                    .withParentEnvironmentType(GeneralCommandLine.ParentEnvironmentType.CONSOLE);
                if (base != null) command.withWorkDirectory(base.toFile());
                return command;
            } catch (IOException e) {
                throw new com.intellij.execution.ExecutionException(e.getMessage(), e);
            }
        }
    }
}
