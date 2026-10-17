package org.ripple.rfl;

import com.intellij.openapi.editor.Document;
import com.intellij.openapi.util.TextRange;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.platform.lsp.api.customization.LspCustomization;
import com.intellij.platform.lsp.api.customization.LspFormattingCustomizer;
import com.intellij.platform.lsp.api.customization.LspFormattingSupport;
import com.intellij.platform.lsp.api.customization.LspRenameCustomizer;
import com.intellij.platform.lsp.api.customization.LspRenameSupport;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

/** Route native editor actions to the compiler's shared language services. */
final class RflLspCustomization extends LspCustomization {
    private final LspFormattingSupport formatting = new LspFormattingSupport() {
        @Override public boolean shouldFormatThisFileExclusivelyByServer(
                @NotNull VirtualFile file, boolean ideCanFormatThisFileItself,
                boolean serverExplicitlyWantsToFormatThisFile) {
            // These flags describe IDE formatting and explicit server selection,
            // not document/range capabilities. Both can be false for our PSI.
            return "rfl".equals(file.getExtension());
        }
    };

    @Override public @NotNull LspFormattingCustomizer getFormattingCustomizer() { return formatting; }

    private final LspRenameSupport rename = new LspRenameSupport() {
        // The default only handles TEXT/TextMate files; RFL has its own PSI.
        @Override public boolean shouldRunRename(@NotNull PsiFile file) {
            return file.getLanguage().is(RflLanguage.INSTANCE);
        }

        @Override public @Nullable TextRange getRenameableRangeAtOffset(@NotNull Document document, int offset) {
            return identifierRange(document.getCharsSequence(), offset);
        }
    };

    @Override public @NotNull LspRenameCustomizer getRenameCustomizer() { return rename; }

    static @Nullable TextRange identifierRange(CharSequence text, int offset) {
        if (offset < 0 || offset > text.length()) return null;
        RflLexer lexer = new RflLexer();
        lexer.start(text);
        TextRange left = null;
        while (lexer.getTokenType() != null) {
            int start = lexer.getTokenStart(), end = lexer.getTokenEnd();
            if (start > offset) break;
            if (lexer.getTokenType() == RflTokens.IDENTIFIER) {
                if (start <= offset && offset < end) return new TextRange(start, end);
                if (end == offset) left = new TextRange(start, end);
            }
            lexer.advance();
        }
        return left;
    }
}
