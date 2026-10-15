package org.ripple.rfl;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.regex.Pattern;

/** Executable discovery shared by the settings page and the LSP launcher. */
final class ServerExecutable {
    private static final boolean WINDOWS = System.getProperty("os.name").startsWith("Windows");
    private static final String NAME = WINDOWS ? "rfl.exe" : "rfl";

    static Path resolve(String configured, Path projectDirectory, String searchPath) throws IOException {
        try {
            String value = configured.strip();
            if (!value.isEmpty()) {
                if (value.startsWith("~/") || value.startsWith("~\\")) {
                    value = System.getProperty("user.home") + value.substring(1);
                }
                Path path = Path.of(value);
                if (path.isAbsolute() || value.contains("/") || value.contains("\\")) {
                    if (!path.isAbsolute()) {
                        if (projectDirectory == null) throw new IOException("Use an absolute executable path when no project directory is available.");
                        path = projectDirectory.resolve(path);
                    }
                    if (executable(path)) return path.toAbsolutePath().normalize();
                    throw new IOException("RFL executable does not exist or is not executable: " + path);
                }
                Path found = inPath(value, searchPath);
                if (found != null) return found;
                throw new IOException("Cannot find '" + value + "' on PATH.");
            }
            Path found = inPath(NAME, searchPath);
            if (found != null) return found;
            // Prefer any ancestor's release build over a nearer debug build.
            // Cargo uses the workspace target directory, even when rfl/ is opened alone.
            for (String profile : new String[] {"release", "debug"}) {
                for (Path directory = projectDirectory; directory != null; directory = directory.getParent()) {
                    Path candidate = directory.resolve("target").resolve(profile).resolve(NAME);
                    if (executable(candidate)) return candidate.toAbsolutePath().normalize();
                }
            }
            throw new IOException("Cannot find rfl. Build it with 'cargo build -p rfl', or set the executable in Settings | Languages & Frameworks | RFL.");
        } catch (InvalidPathException e) {
            throw new IOException("Invalid RFL executable path: " + e.getInput(), e);
        }
    }

    private static Path inPath(String name, String searchPath) {
        if (searchPath == null) return null;
        for (String entry : searchPath.split(Pattern.quote(System.getProperty("path.separator")))) {
            if (entry.isBlank()) continue;
            Path candidate = Path.of(entry).resolve(name);
            if (executable(candidate)) return candidate.toAbsolutePath().normalize();
            if (WINDOWS && !name.endsWith(".exe") && executable(candidate.resolveSibling(name + ".exe"))) {
                return candidate.resolveSibling(name + ".exe").toAbsolutePath().normalize();
            }
        }
        return null;
    }

    private static boolean executable(Path path) {
        return Files.isRegularFile(path) && Files.isExecutable(path);
    }

    private ServerExecutable() {}
}
