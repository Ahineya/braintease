package org.ripple.rfl;

import com.intellij.execution.actions.ConfigurationContext;
import com.intellij.execution.actions.LazyRunConfigurationProducer;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.util.Ref;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.NotNull;

public final class RflRunConfigurationProducer extends LazyRunConfigurationProducer<RflRunConfiguration> implements DumbAware {
    @Override public @NotNull ConfigurationFactory getConfigurationFactory() { return RflRunConfigurationType.factory(); }

    @Override protected boolean setupConfigurationFromContext(@NotNull RflRunConfiguration configuration,
                                                             @NotNull ConfigurationContext context,
                                                             @NotNull Ref<PsiElement> sourceElement) {
        PsiFile file = runnableFile(context);
        if (file == null) return false;
        RflCommand.Options options = configuration.commandOptions().copy();
        options.source = file.getVirtualFile().getPath();
        options.mode = "run";
        options.output = "";
        options.emitIr = false;
        configuration.setCommandOptions(options);
        configuration.setGeneratedName();
        sourceElement.set(file.findElementAt(RflEntryPoint.offset(file.getViewProvider().getContents())));
        return true;
    }

    @Override public boolean isConfigurationFromContext(@NotNull RflRunConfiguration configuration, @NotNull ConfigurationContext context) {
        PsiFile file = runnableFile(context);
        if (file == null || !configuration.commandOptions().mode.equals("run")) return false;
        return configuration.matchesSource(file.getVirtualFile().getPath());
    }

    private static PsiFile runnableFile(ConfigurationContext context) {
        PsiElement element = context.getPsiLocation();
        PsiFile file = element == null ? null : element.getContainingFile();
        VirtualFile virtual = file == null ? null : file.getVirtualFile();
        return virtual != null && virtual.isInLocalFileSystem() && file.getLanguage() == RflLanguage.INSTANCE
            && RflEntryPoint.offset(file.getViewProvider().getContents()) >= 0 ? file : null;
    }
}
