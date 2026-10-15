package org.ripple.rfl;

import com.intellij.openapi.fileTypes.LanguageFileType;
import com.intellij.openapi.util.IconLoader;
import org.jetbrains.annotations.NotNull;

import javax.swing.Icon;

public final class RflFileType extends LanguageFileType {
    public static final RflFileType INSTANCE = new RflFileType();
    public static final Icon ICON = IconLoader.getIcon("/icons/rfl.svg", RflFileType.class);

    private RflFileType() { super(RflLanguage.INSTANCE); }
    @Override public @NotNull String getName() { return "RFL"; }
    @Override public @NotNull String getDescription() { return "Ripple Functional Language"; }
    @Override public @NotNull String getDefaultExtension() { return "rfl"; }
    @Override public Icon getIcon() { return ICON; }
}
