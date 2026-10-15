package org.ripple.rfl;

import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;

/** Finds top-level fn main, ignoring strings, comments and nested declarations. */
final class RflEntryPoint {
    static int offset(CharSequence text) {
        RflLexer lexer = new RflLexer();
        lexer.start(text);
        int depth = 0, stage = 0, nameOffset = -1;
        while (lexer.getTokenType() != null) {
            IElementType token = lexer.getTokenType();
            if (token != TokenType.WHITE_SPACE && !RflTokens.COMMENTS.contains(token)) {
                String word = text.subSequence(lexer.getTokenStart(), lexer.getTokenEnd()).toString();
                if (depth == 0) {
                    if (stage == 2 && token == RflTokens.LPAREN) return nameOffset;
                    if (stage == 1 && token == RflTokens.IDENTIFIER && word.equals("main")) {
                        stage = 2;
                        nameOffset = lexer.getTokenStart();
                    } else {
                        stage = token == RflTokens.KEYWORD && word.equals("fn") ? 1 : 0;
                    }
                }
                if (token == RflTokens.LBRACE) depth++;
                if (token == RflTokens.RBRACE) depth = Math.max(0, depth - 1);
            }
            lexer.advance();
        }
        return -1;
    }

    private RflEntryPoint() {}
}
