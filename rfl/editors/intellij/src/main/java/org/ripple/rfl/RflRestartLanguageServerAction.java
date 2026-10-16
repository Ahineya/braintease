package org.ripple.rfl;

import com.intellij.openapi.actionSystem.ActionUpdateThread;
import com.intellij.openapi.actionSystem.AnActionEvent;
import com.intellij.openapi.project.DumbAwareAction;
import com.intellij.openapi.project.Project;
import com.intellij.platform.lsp.api.LspClientManager;
import org.jetbrains.annotations.NotNull;

public final class RflRestartLanguageServerAction extends DumbAwareAction {
    @Override public @NotNull ActionUpdateThread getActionUpdateThread() {
        return ActionUpdateThread.BGT;
    }

    @Override public void update(@NotNull AnActionEvent event) {
        Project project = event.getProject();
        event.getPresentation().setEnabled(project != null && !project.isDisposed());
    }

    @Override public void actionPerformed(@NotNull AnActionEvent event) {
        Project project = event.getProject();
        if (project != null && !project.isDisposed()) {
            LspClientManager.getInstance(project).stopAndRestartClientsIfNeeded(RflLspSupportProvider.class);
        }
    }
}
