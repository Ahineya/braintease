package org.ripple.rfl;

import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors;
import com.intellij.openapi.editor.HighlighterColors;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.*;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;

public final class RflSyntaxHighlighter extends SyntaxHighlighterBase {
    @Override public @NotNull Lexer getHighlightingLexer() { return new RflHighlightingLexer(); }
    @Override public TextAttributesKey @NotNull [] getTokenHighlights(IElementType token) {
        TextAttributesKey key = null;
        if (token == RflTokens.HTML_TAG || token == RflTokens.HTML_DELIMITER) key = DefaultLanguageHighlighterColors.MARKUP_TAG;
        else if (token == RflTokens.HTML_ATTRIBUTE) key = DefaultLanguageHighlighterColors.MARKUP_ATTRIBUTE;
        else if (token == RflTokens.HTML_ENTITY) key = DefaultLanguageHighlighterColors.MARKUP_ENTITY;
        else if (token == RflTokens.HTML_COMMENT) key = DefaultLanguageHighlighterColors.BLOCK_COMMENT;
        else if (token == RflTokens.KEYWORD) key = DefaultLanguageHighlighterColors.KEYWORD;
        else if (token == RflTokens.TYPE) key = DefaultLanguageHighlighterColors.PREDEFINED_SYMBOL;
        else if (token == RflTokens.NUMBER) key = DefaultLanguageHighlighterColors.NUMBER;
        else if (RflTokens.STRINGS.contains(token)) key = DefaultLanguageHighlighterColors.STRING;
        else if (token == RflTokens.LINE_COMMENT) key = DefaultLanguageHighlighterColors.LINE_COMMENT;
        else if (token == RflTokens.BLOCK_COMMENT) key = DefaultLanguageHighlighterColors.BLOCK_COMMENT;
        else if (token == RflTokens.OPERATOR) key = DefaultLanguageHighlighterColors.OPERATION_SIGN;
        else if (token == RflTokens.LBRACE || token == RflTokens.RBRACE) key = DefaultLanguageHighlighterColors.BRACES;
        else if (token == RflTokens.LPAREN || token == RflTokens.RPAREN) key = DefaultLanguageHighlighterColors.PARENTHESES;
        else if (token == RflTokens.LBRACKET || token == RflTokens.RBRACKET) key = DefaultLanguageHighlighterColors.BRACKETS;
        else if (token == TokenType.BAD_CHARACTER) key = HighlighterColors.BAD_CHARACTER;
        return pack(key);
    }

    public static final class Factory extends SyntaxHighlighterFactory {
        @Override public @NotNull SyntaxHighlighter getSyntaxHighlighter(Project project, VirtualFile file) {
            return new RflSyntaxHighlighter();
        }
    }
}
