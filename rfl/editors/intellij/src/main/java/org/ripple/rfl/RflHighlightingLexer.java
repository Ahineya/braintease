package org.ripple.rfl;

import com.intellij.lexer.LexerBase;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Highlighting layer only: the parser continues to see lossless macro tokens. */
final class RflHighlightingLexer extends LexerBase {
    private record Piece(IElementType kind, int start, int end) {}
    private final RflLexer base = new RflLexer();
    private List<Piece> pieces = List.of();
    private Set<String> names = Set.of();
    private int index, macroStart;

    @Override public void start(@NotNull CharSequence text, int start, int end, int state) {
        names = htmlNames(text, end);
        // An interior token records its containing macro's start. Replaying one
        // opaque group restores all HTML/RFL contexts, including nested strings.
        base.start(text, state == 0 ? start : state - 1, end, 0);
        prepare();
        while (getTokenType() != null && getTokenEnd() <= start) advance();
    }
    @Override public int getState() { return pieces.isEmpty() ? 0 : macroStart + 1; }
    @Override public IElementType getTokenType() { return pieces.isEmpty() ? base.getTokenType() : pieces.get(index).kind(); }
    @Override public int getTokenStart() { return pieces.isEmpty() ? base.getTokenStart() : pieces.get(index).start(); }
    @Override public int getTokenEnd() { return pieces.isEmpty() ? base.getTokenEnd() : pieces.get(index).end(); }
    @Override public @NotNull CharSequence getBufferSequence() { return base.getBufferSequence(); }
    @Override public int getBufferEnd() { return base.getBufferEnd(); }
    @Override public void advance() {
        if (!pieces.isEmpty() && ++index < pieces.size()) return;
        base.advance(); prepare();
    }
    private void prepare() {
        pieces = List.of(); index = 0;
        if (base.getTokenType() != RflTokens.MACRO_BODY) return;
        macroStart = base.getTokenStart();
        if (!names.contains(macroName(base.getBufferSequence(), macroStart))) return;
        var out = new ArrayList<Piece>();
        html(base.getBufferSequence(), macroStart, base.getTokenEnd(), out, 0, names);
        pieces = out;
    }
    private static String macroName(CharSequence text, int at) {
        int p = before(text, at);
        if (p < 0 || text.charAt(p) != '!') return "";
        int end = before(text, p) + 1, begin = end;
        while (begin > 0 && word(text.charAt(begin - 1))) begin--;
        return text.subSequence(begin, end).toString();
    }
    private static int before(CharSequence text, int at) {
        int p = at - 1;
        while (p >= 0) {
            if (Character.isWhitespace(text.charAt(p))) { p--; continue; }
            if (p > 0 && text.charAt(p) == '/' && text.charAt(p - 1) == '*') {
                p -= 2;
                while (p > 0 && !(text.charAt(p - 1) == '/' && text.charAt(p) == '*')) p--;
                p -= 2; continue;
            }
            break;
        }
        return p;
    }
    private static Set<String> htmlNames(CharSequence text, int end) {
        var result = new HashSet<String>();
        // Lexical colouring recognizes conventional html!, and aliases imported
        // from the standard DSL. This never resolves or executes an import.
        result.add("html");
        var lexer = new RflLexer(); lexer.start(text, 0, end, 0);
        var parts = new ArrayList<String>(); boolean importing = false;
        while (lexer.getTokenType() != null) {
            var kind = lexer.getTokenType();
            String token = text.subSequence(lexer.getTokenStart(), lexer.getTokenEnd()).toString();
            if (kind == RflTokens.KEYWORD && token.equals("import")) { importing = true; parts.clear(); }
            if (importing && kind != TokenType.WHITE_SPACE && !RflTokens.COMMENTS.contains(kind)) {
                parts.add(token);
                if (kind == RflTokens.STRING) {
                    if (parts.size() > 3 && parts.get(1).equals("macro")
                        && (token.equals("\"std/html_macro\"") || token.equals("\"std/html_macro.rfl\""))) {
                        for (int i = 2; i < parts.size() - 1; i++) {
                            if (parts.get(i).equals("html")) result.add(i + 2 < parts.size() && parts.get(i + 1).equals("as") ? parts.get(i + 2) : "html");
                        }
                    }
                    importing = false;
                }
            }
            lexer.advance();
        }
        return result;
    }
    private static void add(List<Piece> out, IElementType kind, int a, int b) {
        if (b > a) out.add(new Piece(kind, a, b));
    }
    private static boolean word(char c) { return Character.isLetterOrDigit(c) || c == '_' || c == '-' || c == ':'; }
    private static boolean starts(CharSequence text, int at, int end, String value) {
        if (at + value.length() > end) return false;
        for (int i = 0; i < value.length(); i++) if (text.charAt(at + i) != value.charAt(i)) return false;
        return true;
    }
    private static IElementType bracket(char c) {
        return switch (c) { case '(' -> RflTokens.LPAREN; case ')' -> RflTokens.RPAREN;
            case '[' -> RflTokens.LBRACKET; case ']' -> RflTokens.RBRACKET;
            case '{' -> RflTokens.LBRACE; default -> RflTokens.RBRACE; };
    }
    private static void html(CharSequence text, int begin, int end, List<Piece> out, int depth, Set<String> names) {
        if (depth > 128) { add(out, RflTokens.MACRO_BODY, begin, end); return; }
        char open = text.charAt(begin), close = open == '{' ? '}' : open == '[' ? ']' : ')';
        boolean closed = end > begin + 1 && text.charAt(end - 1) == close;
        int limit = closed ? end - 1 : end, p = begin + 1;
        add(out, bracket(open), begin, p);
        boolean tag = false, tagName = false;
        while (p < limit) {
            int a = p; char c = text.charAt(p);
            if (starts(text, p, limit, "<!--") || starts(text, p, limit, "/*") || starts(text, p, limit, "//")) {
                String finish = starts(text, p, limit, "<!--") ? "-->" : starts(text, p, limit, "/*") ? "*/" : "\n";
                p += finish.equals("-->") ? 4 : 2;
                if (finish.equals("*/")) {
                    int comments = 1;
                    while (p < limit && comments > 0) {
                        if (starts(text, p, limit, "/*")) { comments++; p += 2; }
                        else if (starts(text, p, limit, "*/")) { comments--; p += 2; }
                        else p++;
                    }
                } else {
                    while (p < limit && !starts(text, p, limit, finish)) p++;
                    p = Math.min(limit, p + finish.length());
                }
                add(out, RflTokens.HTML_COMMENT, a, p);
            } else if (c == '{') {
                var code = new RflLexer(); code.start(text, p, limit, 0); int braces = 0;
                while (code.getTokenType() != null) {
                    var kind = code.getTokenType();
                    if (kind == RflTokens.LBRACE) braces++;
                    if (kind == RflTokens.RBRACE) braces--;
                    if (kind == RflTokens.MACRO_BODY && names.contains(macroName(text, code.getTokenStart()))) {
                        html(text, code.getTokenStart(), code.getTokenEnd(), out, depth + 1, names);
                    } else { add(out, kind, code.getTokenStart(), code.getTokenEnd()); }
                    p = code.getTokenEnd();
                    if (braces == 0) break;
                    code.advance();
                }
            } else if (c == '<') {
                tag = true; tagName = true; p++;
                if (p < limit && text.charAt(p) == '/') p++;
                add(out, RflTokens.HTML_DELIMITER, a, p);
            } else if (tag && (c == '>' || c == '/')) {
                p++; if (c == '/' && p < limit && text.charAt(p) == '>') p++;
                tag = false; add(out, RflTokens.HTML_DELIMITER, a, p);
            } else if (tag && (c == '"' || c == '\'')) {
                p++; while (p < limit) { char ch = text.charAt(p++); if (ch == c) break; if (ch == '\\' && p < limit) p++; }
                add(out, RflTokens.STRING, a, p);
            } else if (Character.isWhitespace(c)) {
                while (p < limit && Character.isWhitespace(text.charAt(p))) p++;
                add(out, TokenType.WHITE_SPACE, a, p);
            } else if (tag && word(c)) {
                while (p < limit && word(text.charAt(p))) p++;
                add(out, tagName ? RflTokens.HTML_TAG : Character.isDigit(c) ? RflTokens.NUMBER : RflTokens.HTML_ATTRIBUTE, a, p); tagName = false;
            } else if (tag) { add(out, RflTokens.OPERATOR, p, ++p); }
            else if (c == '&') {
                p++; if (p < limit && text.charAt(p) == '#') p++;
                while (p < limit && word(text.charAt(p))) p++;
                if (p < limit && text.charAt(p) == ';') p++;
                add(out, RflTokens.HTML_ENTITY, a, p);
            } else {
                p++; while (p < limit && text.charAt(p) != '<' && text.charAt(p) != '{' && text.charAt(p) != '&' && !Character.isWhitespace(text.charAt(p)) && !starts(text,p,limit,"/*") && !starts(text,p,limit,"//")) p++;
                add(out, RflTokens.HTML_TEXT, a, p);
            }
        }
        if (closed) add(out, bracket(close), limit, end);
    }
}
