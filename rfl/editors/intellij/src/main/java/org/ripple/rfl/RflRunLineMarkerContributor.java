package org.ripple.rfl;

import com.intellij.execution.lineMarker.ExecutorAction;
import com.intellij.execution.lineMarker.RunLineMarkerContributor;
import com.intellij.icons.AllIcons;
import com.intellij.openapi.project.DumbAware;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.NotNull;

public final class RflRunLineMarkerContributor extends RunLineMarkerContributor implements DumbAware {
    @Override public Info getInfo(@NotNull PsiElement element) {
        if (element.getFirstChild() != null || !element.textMatches("main")) return null;
        PsiFile file = element.getContainingFile();
        if (file == null || file.getVirtualFile() == null || !file.getVirtualFile().isInLocalFileSystem()) return null;
        if (element.getTextRange().getStartOffset() != RflEntryPoint.offset(file.getViewProvider().getContents())) return null;
        return new Info(AllIcons.RunConfigurations.TestState.Run, ExecutorAction.getActions(0), ignored -> "Run RFL program");
    }
}
