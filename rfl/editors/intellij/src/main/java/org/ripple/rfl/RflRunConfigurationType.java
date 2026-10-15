package org.ripple.rfl;

import com.intellij.execution.configurations.*;
import com.intellij.openapi.project.Project;
import org.jetbrains.annotations.NotNull;

public final class RflRunConfigurationType extends ConfigurationTypeBase {
    public RflRunConfigurationType() {
        super("RFL", "RFL", "Run, compile or check an RFL source file", RflFileType.ICON);
        addFactory(new ConfigurationFactory(this) {
            @Override public @NotNull String getId() { return "RFL"; }
            @Override public @NotNull RunConfiguration createTemplateConfiguration(@NotNull Project project) {
                return new RflRunConfiguration(project, this, "RFL");
            }
            @Override public boolean isEditableInDumbMode() { return true; }
        });
    }

    static ConfigurationFactory factory() {
        return ConfigurationTypeUtil.findConfigurationType(RflRunConfigurationType.class).getConfigurationFactories()[0];
    }
}
