package org.ripple.rfl;

import com.intellij.lang.Language;

public final class RflLanguage extends Language {
    public static final RflLanguage INSTANCE = new RflLanguage();

    private RflLanguage() {
        super("RFL");
    }
}
