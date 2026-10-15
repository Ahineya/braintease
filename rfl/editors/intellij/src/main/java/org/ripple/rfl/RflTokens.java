package org.ripple.rfl;

import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.IFileElementType;
import com.intellij.psi.tree.TokenSet;

final class RflTokens {
    static final IFileElementType FILE = new IFileElementType(RflLanguage.INSTANCE);
    static final IElementType IDENTIFIER = token("IDENTIFIER");
    static final IElementType KEYWORD = token("KEYWORD");
    static final IElementType TYPE = token("TYPE");
    static final IElementType NUMBER = token("NUMBER");
    static final IElementType STRING = token("STRING");
    static final IElementType CHARACTER = token("CHARACTER");
    static final IElementType LINE_COMMENT = token("LINE_COMMENT");
    static final IElementType BLOCK_COMMENT = token("BLOCK_COMMENT");
    static final IElementType OPERATOR = token("OPERATOR");
    static final IElementType PUNCTUATION = token("PUNCTUATION");
    static final IElementType LPAREN = token("LPAREN"), RPAREN = token("RPAREN");
    static final IElementType LBRACE = token("LBRACE"), RBRACE = token("RBRACE");
    static final IElementType LBRACKET = token("LBRACKET"), RBRACKET = token("RBRACKET");
    static final TokenSet COMMENTS = TokenSet.create(LINE_COMMENT, BLOCK_COMMENT);
    static final TokenSet STRINGS = TokenSet.create(STRING, CHARACTER);

    private static IElementType token(String name) { return new IElementType(name, RflLanguage.INSTANCE); }
    private RflTokens() {}
}
