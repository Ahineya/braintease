package org.ripple.rfl;

import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Small dependency-free checks, run by build.py --test or Gradle checkEditor. */
public final class EditorTest {
    public static void main(String[] args) throws Exception {
        lexer();
        rename();
        formatting();
        traits();
        consumingCallables();
        comprehensions();
        macros();
        htmlHighlighting();
        Path build = Path.of(args.length == 0 ? "build" : args[0]);
        Files.createDirectories(build);
        Path temporary = Files.createTempDirectory(build, "editor-test-");
        try {
            // Bound ancestor discovery to the fixture, away from real workspace binaries.
            discovery(Path.of("").toAbsolutePath().relativize(temporary.toAbsolutePath()));
        } finally {
            try (var files = Files.walk(temporary)) {
                for (Path path : files.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
            }
        }
        System.out.println("Editor checks passed: lexer recovery/restarts, bracket/comment isolation, LSP formatting, executable discovery.");
    }

    private static void lexer() {
        List<IElementType> kinds = tokens("fn main() -> i16 { let s = c\"[\\\"]\"; // }\n/* (\n) */ 'x' 1..2 1.5e-2 }");
        require(kinds.get(0) == RflTokens.KEYWORD, "fn keyword");
        require(kinds.stream().filter(t -> t == RflTokens.LBRACKET).count() == 0, "brackets in strings are not code");
        require(kinds.stream().filter(t -> t == RflTokens.RBRACE).count() == 1, "braces in comments are not code");
        require(kinds.contains(RflTokens.BLOCK_COMMENT), "multiline block comment");
        require(kinds.contains(RflTokens.CHARACTER), "character literal");
        require(kinds.stream().filter(t -> t == RflTokens.NUMBER).count() == 3, "range versus fractional/exponent numbers");
        require(tokens("\"unfinished\nfn").equals(List.of(RflTokens.STRING, RflTokens.KEYWORD)), "unterminated string recovers at newline");
        require(tokens("/* unfinished").equals(List.of(RflTokens.BLOCK_COMMENT)), "unterminated comment extends to EOF");
        require(tokens("🙂").equals(List.of(TokenType.BAD_CHARACTER, TokenType.BAD_CHARACTER)), "invalid Unicode always advances");
        // Check every token boundary can be used to restart incremental highlighting.
        String text = "/* two\nlines */ async fn x(f: Async<i16>) { c\"hi\"; '\\n'; f.await }";
        RflLexer full = new RflLexer();
        full.start(text);
        while (full.getTokenType() != null) {
            RflLexer restarted = new RflLexer();
            restarted.start(text, full.getTokenStart(), text.length(), full.getState());
            require(restarted.getTokenType() == full.getTokenType() && restarted.getTokenEnd() == full.getTokenEnd(), "lexer restart");
            full.advance();
        }
    }

    private static void formatting() {
        var support = new RflLspCustomization().getFormattingCustomizer();
        require(support instanceof com.intellij.platform.lsp.api.customization.LspFormattingSupport,
            "native Reformat Code uses the language server formatter");
        var formatter = (com.intellij.platform.lsp.api.customization.LspFormattingSupport) support;
        var file = new com.intellij.testFramework.LightVirtualFile("example.rfl");
        require(formatter.shouldFormatThisFileExclusivelyByServer(file, true, false), "prefer the shared formatter over IDE formatting");
        require(formatter.shouldFormatThisFileExclusivelyByServer(file, false, true), "explicit server formatting selection");
        require(formatter.shouldFormatThisFileExclusivelyByServer(file, false, false),
            "RFL PSI without a built-in formatter or explicit server selection still uses LSP");
        require(!formatter.shouldFormatThisFileExclusivelyByServer(
            new com.intellij.testFramework.LightVirtualFile("other.txt"), true, true), "only claim RFL files");
    }

    private static void rename() {
        var support = new RflLspCustomization().getRenameCustomizer();
        require(support instanceof com.intellij.platform.lsp.api.customization.LspRenameSupport, "native LSP rename enabled");
        var file = (com.intellij.psi.PsiFile) java.lang.reflect.Proxy.newProxyInstance(
            EditorTest.class.getClassLoader(), new Class<?>[] {com.intellij.psi.PsiFile.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("getLanguage")) return RflLanguage.INSTANCE;
                throw new UnsupportedOperationException(method.getName());
            });
        require(((com.intellij.platform.lsp.api.customization.LspRenameSupport) support).shouldRunRename(file), "rename handles RFL PSI instead of only plain text");
        String text = "// frequency\nfn main() { let frequency = 1; println(\"frequency\"); frequency }";
        int start = text.indexOf("frequency =");
        for (int caret : new int[] {start, start + 3, start + "frequency".length()}) {
            var range = RflLspCustomization.identifierRange(text, caret);
            require(range != null && range.getStartOffset() == start && range.getLength() == "frequency".length(), "identifier and end-caret range");
        }
        for (int caret : new int[] {-1, text.length() + 1, 4, text.indexOf("fn"), text.indexOf("1;"), text.indexOf("\"frequency\"") + 3}) {
            require(RflLspCustomization.identifierRange(text, caret) == null, "comments, strings, keywords and numbers cannot rename");
        }
    }

    private static void traits() {
        String text = "trait Printable { fn print(&self); } impl<T: Printable + Debug> Printable for Vec<T> { fn print(&self) { Printable::print(self); } }";
        require(tokens("trait").equals(List.of(RflTokens.KEYWORD)), "trait keyword");
        require(tokens("Printable").equals(List.of(RflTokens.IDENTIFIER)), "user traits remain identifiers for semantic classification");
        require(!tokens(text).contains(TokenType.BAD_CHARACTER), "traits, multiple bounds, impls and qualified calls tokenize");
        require(RflLspCustomization.identifierRange(text, 1) == null, "trait keyword cannot rename");
        for (String name : new String[] {"Printable", "print", "Debug"}) {
            int start = text.indexOf(name);
            var range = RflLspCustomization.identifierRange(text, start + 1);
            require(range != null && range.getStartOffset() == start && range.getLength() == name.length(), "trait, method and bound rename range");
        }
    }

    private static void consumingCallables() {
        require(tokens("once").equals(List.of(RflTokens.IDENTIFIER)), "once remains a contextual identifier");
        require(tokens("once fn").equals(List.of(RflTokens.IDENTIFIER, RflTokens.KEYWORD)), "consuming function modifier and fn tokenize");
        String text = "fn call(f: once fn(i16) -> i16) -> i16 { f(42) } fn main() { let once = 1; let f = once fn(x: i16) => x; }";
        require(!tokens(text).contains(TokenType.BAD_CHARACTER), "consuming types and lambda syntax tokenize");
        int start = text.indexOf("once =");
        require(RflLspCustomization.identifierRange(text, start + 1).getStartOffset() == start, "ordinary once binding remains renameable");
    }

    private static void comprehensions() {
        String text = "fn main(){let yield=1;let p=for once{n<-Some(yield);let m=n+1;yield m};}";
        require(!tokens(text).contains(TokenType.BAD_CHARACTER), "comprehensions and arrows tokenize");
        require(tokens("yield").equals(List.of(RflTokens.IDENTIFIER)), "yield remains contextual");
        int start = text.indexOf("yield=");
        require(RflLspCustomization.identifierRange(text, start + 1).getStartOffset() == start,
            "ordinary yield binding remains renameable");
    }

    private static void macros() {
        String text = "fn main(){let view=html!{<p>isn't ordinary @ RFL 🙂 {name}</p>};}";
        require(tokens("macro").equals(List.of(RflTokens.IDENTIFIER)), "macro remains contextual");
        require(!tokens(text).contains(TokenType.BAD_CHARACTER), "raw DSL is an opaque lossless group");
        require(tokens(text).contains(RflTokens.MACRO_BODY), "macro group captured");
        int start = text.indexOf("name}");
        require(RflLspCustomization.identifierRange(text,start+1).getStartOffset() == start, "interpolations can ask server prepareRename");
        RflLexer full = new RflLexer(); full.start(text);
        while (full.getTokenType() != null) {
            RflLexer restarted = new RflLexer(); restarted.start(text,full.getTokenStart(),text.length(),full.getState());
            require(restarted.getTokenType()==full.getTokenType() && restarted.getTokenEnd()==full.getTokenEnd(), "macro lexer restart");full.advance();
        }
        require(tokens("html!{unfinished isn't @ 🙂").contains(RflTokens.MACRO_BODY), "incomplete raw input remains tolerant");
    }

    private static void htmlHighlighting() {
        for (String text : new String[] {
            "fn view(){html!{<div class=\"x\">isn't 🙂 &amp; {fn(x:i16)=>{x+1}}<input value={name}/></div>};}",
            "import macro {html as view} from \"std/html_macro\";fn main(){view!{<><svg><path d=\"M0\"/></svg></>};}",
            "import macro {html as render} from \"std/html_macro\";fn main(){html!{<div>{render!{<section>nested</section>}}</div>};}",
            "html!{<div><!-- comment -->{html!{<b>{name}</b>}}</div>}",
            "html!{<Badge count=42>/* outer /* inner */ } */{name}</Badge>}",
            "html!{<div title=\"unfinished", "html!{<div>{fn(s:String)=>{s", "raw!{<opaque @ 🙂>}",
        }) {
            var lexer = new RflSyntaxHighlighter().getHighlightingLexer(); lexer.start(text);
            int end = 0; boolean tag = false, attribute = false, rfl = false;
            while (lexer.getTokenType() != null) {
                require(lexer.getTokenStart() == end && lexer.getTokenEnd() > end, "HTML coverage and progress");
                end = lexer.getTokenEnd();
                if (text.contains("html as render")) require(lexer.getTokenType() != RflTokens.MACRO_BODY, "nested HTML alias is highlighted");
                String spelling = text.substring(lexer.getTokenStart(), lexer.getTokenEnd());
                if (spelling.startsWith("/* outer")) require(lexer.getTokenType() == RflTokens.HTML_COMMENT && spelling.endsWith("} */"), "nested DSL comments");
                if (spelling.equals("42")) require(lexer.getTokenType() == RflTokens.NUMBER, "component numeric property");
                tag |= lexer.getTokenType() == RflTokens.HTML_TAG;
                attribute |= lexer.getTokenType() == RflTokens.HTML_ATTRIBUTE;
                rfl |= lexer.getTokenType() == RflTokens.KEYWORD;
                var restarted = new RflSyntaxHighlighter().getHighlightingLexer();
                restarted.start(text, lexer.getTokenStart(), text.length(), lexer.getState());
                require(restarted.getTokenType() == lexer.getTokenType() && restarted.getTokenEnd() == end, "HTML incremental restart");
                lexer.advance();
            }
            require(end == text.length(), "HTML lossless coverage");
            if (text.startsWith("fn view")) require(tag && attribute && rfl, "HTML tags/attributes and embedded RFL highlighted separately");
            if (text.contains("html as view")) require(tag, "HTML macro import alias");
        }
        require(new RflSyntaxHighlighter().getTokenHighlights(RflTokens.HTML_TAG).length > 0, "HTML theme colours");
    }

    private static List<IElementType> tokens(String text) {
        RflLexer lexer = new RflLexer();
        lexer.start(text);
        List<IElementType> kinds = new ArrayList<>();
        int end = 0;
        while (lexer.getTokenType() != null) {
            require(lexer.getTokenStart() == end && lexer.getTokenEnd() > end, "complete progressing token coverage");
            end = lexer.getTokenEnd();
            if (lexer.getTokenType() != TokenType.WHITE_SPACE) kinds.add(lexer.getTokenType());
            lexer.advance();
        }
        require(end == text.length(), "complete source coverage");
        return kinds;
    }

    private static void discovery(Path root) throws Exception {
        String name = System.getProperty("os.name").startsWith("Windows") ? "rfl.exe" : "rfl";
        Path workspace = root.resolve("project with spaces");
        Path project = Files.createDirectories(workspace.resolve("rfl/examples"));
        Path debug = executable(workspace.resolve("target/debug/" + name));
        Path release = executable(workspace.resolve("target/release/" + name));
        Path onPath = executable(root.resolve("bin/" + name));
        require(ServerExecutable.resolve("", project, onPath.getParent().toString()).equals(onPath), "PATH precedes workspace release and debug builds");
        require(ServerExecutable.resolve("", project, "").equals(release), "release precedes debug when PATH has no rfl");
        require(ServerExecutable.resolve(release.toString(), project, "").equals(release), "explicit path overrides discovery, including spaces");
        require(ServerExecutable.resolve(debug.toString(), project, onPath.getParent().toString()).equals(debug), "explicit path overrides PATH and release preference");
        require(ServerExecutable.resolve("../../target/release/" + name, project, "").equals(release), "project-relative executable");
        require(ServerExecutable.resolve(name, project, onPath.getParent().toString()).equals(onPath), "explicit command searches PATH");
        Path nearerDebug = executable(project.resolve("target/debug/" + name));
        require(ServerExecutable.resolve("", project, "").equals(release), "ancestor release precedes nearer debug");
        Files.delete(release);
        require(ServerExecutable.resolve("", project, "").equals(nearerDebug), "debug fallback after all release candidates are absent");
        Files.delete(nearerDebug);
        require(ServerExecutable.resolve("", project, "").equals(debug), "ancestor debug fallback");
        Files.delete(debug);
        require(ServerExecutable.resolve("", null, onPath.getParent().toString()).equals(onPath), "PATH fallback without a workspace");
        try {
            ServerExecutable.resolve("", null, "");
            throw new AssertionError("Missing executable must fail");
        } catch (IOException expected) {
            require(expected.getMessage().contains("Settings"), "missing executable provides recovery instructions");
        }
        try {
            ServerExecutable.resolve(workspace.resolve("does-not-exist").toString(), project, onPath.getParent().toString());
            throw new AssertionError("Explicit invalid path must not silently fall back");
        } catch (IOException expected) {
            require(expected.getMessage().contains("not executable"), "invalid explicit executable reported");
        }
    }

    private static Path executable(Path path) throws IOException {
        Files.createDirectories(path.getParent());
        Files.writeString(path, "placeholder");
        require(path.toFile().setExecutable(true), "make fixture executable");
        return path.toAbsolutePath().normalize();
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
