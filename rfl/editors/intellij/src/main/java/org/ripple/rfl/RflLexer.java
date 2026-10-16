package org.ripple.rfl;

import com.intellij.lexer.LexerBase;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;

import java.util.Set;

/** Tolerant editor lexer. Parsing and semantic analysis remain in rfl lsp. */
public final class RflLexer extends LexerBase {
    private static final Set<String> KEYWORDS = Set.of(
        "fn", "let", "mut", "if", "else", "return", "as", "while", "break", "continue",
        "match", "for", "in", "type", "trait", "impl", "const", "struct", "export", "import", "extern",
        "unsafe", "true", "false");
    private static final Set<String> TYPES = Set.of(
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64", "bool", "char",
        "String", "usize", "unit", "Vec", "Slice", "Owned", "Option", "Result", "c_int", "c_char",
        "c_short", "c_uint", "c_long", "c_size_t");
    private CharSequence buffer = "";
    private int start, end, limit;
    private IElementType type;

    @Override public void start(@NotNull CharSequence text, int startOffset, int endOffset, int initialState) {
        buffer = text;
        start = end = startOffset;
        limit = endOffset;
        advance();
    }

    // Every token includes its complete literal/comment, so restarts occur at token boundaries.
    @Override public int getState() { return 0; }
    @Override public IElementType getTokenType() { return type; }
    @Override public int getTokenStart() { return start; }
    @Override public int getTokenEnd() { return end; }
    @Override public @NotNull CharSequence getBufferSequence() { return buffer; }
    @Override public int getBufferEnd() { return limit; }

    @Override public void advance() {
        start = end;
        type = null;
        if (end >= limit) return;
        char c = buffer.charAt(end++);
        if (Character.isWhitespace(c)) {
            while (end < limit && Character.isWhitespace(buffer.charAt(end))) end++;
            type = TokenType.WHITE_SPACE;
        } else if (c == '/' && at('/')) {
            while (end < limit && !at('\n') && !at('\r')) end++;
            type = RflTokens.LINE_COMMENT;
        } else if (c == '/' && at('*')) {
            end++;
            while (end < limit) {
                if (at('*') && end + 1 < limit && buffer.charAt(end + 1) == '/') {
                    end += 2;
                    break;
                }
                end++;
            }
            type = RflTokens.BLOCK_COMMENT;
        } else if (c == '"' || c == '\'' || (c == 'c' && at('"'))) {
            char quote = c == 'c' ? '"' : c;
            if (c == 'c') end++;
            while (end < limit && !at('\n') && !at('\r')) {
                char ch = buffer.charAt(end++);
                if (ch == quote) break;
                if (ch == '\\' && end < limit && !at('\n') && !at('\r')) end++;
            }
            type = quote == '\'' ? RflTokens.CHARACTER : RflTokens.STRING;
        } else if (digit(c)) {
            while (end < limit && digit(buffer.charAt(end))) end++;
            if (at('.') && end + 1 < limit && digit(buffer.charAt(end + 1))) {
                end++;
                while (end < limit && digit(buffer.charAt(end))) end++;
            }
            if (at('e') || at('E')) {
                end++;
                if (at('+') || at('-')) end++;
                while (end < limit && digit(buffer.charAt(end))) end++;
            }
            type = RflTokens.NUMBER;
        } else if (identifierStart(c)) {
            while (end < limit && (identifierStart(buffer.charAt(end)) || digit(buffer.charAt(end)))) end++;
            String word = buffer.subSequence(start, end).toString();
            type = KEYWORDS.contains(word) ? RflTokens.KEYWORD : TYPES.contains(word) ? RflTokens.TYPE : RflTokens.IDENTIFIER;
        } else {
            type = switch (c) {
                case '(' -> RflTokens.LPAREN;
                case ')' -> RflTokens.RPAREN;
                case '{' -> RflTokens.LBRACE;
                case '}' -> RflTokens.RBRACE;
                case '[' -> RflTokens.LBRACKET;
                case ']' -> RflTokens.RBRACKET;
                case ',', ':', ';', '.', '#', '?' -> RflTokens.PUNCTUATION;
                case '+', '-', '*', '/', '%', '&', '|', '^', '~', '!', '=', '<', '>' -> RflTokens.OPERATOR;
                default -> TokenType.BAD_CHARACTER;
            };
        }
    }

    private boolean at(char c) { return end < limit && buffer.charAt(end) == c; }
    private static boolean digit(char c) { return c >= '0' && c <= '9'; }
    private static boolean identifierStart(char c) { return c == '_' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'; }
}
