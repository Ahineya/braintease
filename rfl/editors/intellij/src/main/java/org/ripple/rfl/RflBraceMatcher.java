package org.ripple.rfl;

import com.intellij.lang.BracePair;
import com.intellij.lang.PairedBraceMatcher;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;

public final class RflBraceMatcher implements PairedBraceMatcher {
    @Override public BracePair @NotNull [] getPairs() {
        return new BracePair[] {
            new BracePair(RflTokens.LPAREN, RflTokens.RPAREN, false),
            new BracePair(RflTokens.LBRACKET, RflTokens.RBRACKET, false),
            new BracePair(RflTokens.LBRACE, RflTokens.RBRACE, true)
        };
    }
    @Override public boolean isPairedBracesAllowedBeforeType(@NotNull IElementType left, IElementType context) { return true; }
    @Override public int getCodeConstructStart(PsiFile file, int openingBraceOffset) { return openingBraceOffset; }
}
