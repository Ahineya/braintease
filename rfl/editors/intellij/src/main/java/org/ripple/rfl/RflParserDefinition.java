package org.ripple.rfl;

import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.extapi.psi.PsiFileBase;
import com.intellij.lang.*;
import com.intellij.lexer.Lexer;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.openapi.project.Project;
import com.intellij.psi.*;
import com.intellij.psi.tree.*;
import org.jetbrains.annotations.NotNull;

public final class RflParserDefinition implements ParserDefinition {
    @Override public @NotNull Lexer createLexer(Project project) { return new RflLexer(); }
    @Override public @NotNull PsiParser createParser(Project project) {
        return (root, builder) -> {
            PsiBuilder.Marker file = builder.mark();
            while (!builder.eof()) builder.advanceLexer();
            file.done(root);
            return builder.getTreeBuilt();
        };
    }
    @Override public @NotNull IFileElementType getFileNodeType() { return RflTokens.FILE; }
    @Override public @NotNull TokenSet getCommentTokens() { return RflTokens.COMMENTS; }
    @Override public @NotNull TokenSet getStringLiteralElements() { return RflTokens.STRINGS; }
    @Override public @NotNull PsiElement createElement(ASTNode node) { return new ASTWrapperPsiElement(node); }
    @Override public @NotNull PsiFile createFile(@NotNull FileViewProvider provider) { return new RflFile(provider); }

    private static final class RflFile extends PsiFileBase {
        RflFile(FileViewProvider provider) { super(provider, RflLanguage.INSTANCE); }
        @Override public @NotNull FileType getFileType() { return RflFileType.INSTANCE; }
    }
}
