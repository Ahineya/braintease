package org.ripple.rfl;

import com.intellij.openapi.options.Configurable;
import com.intellij.openapi.options.ConfigurationException;
import com.intellij.openapi.project.Project;
import com.intellij.platform.lsp.api.LspClientManager;
import com.intellij.util.EnvironmentUtil;

import javax.swing.*;
import java.awt.BorderLayout;
import java.io.IOException;
import java.nio.file.Path;

public final class RflConfigurable implements Configurable {
    private final Project project;
    private JTextField executable;

    public RflConfigurable(Project project) { this.project = project; }
    @Override public String getDisplayName() { return "RFL"; }

    @Override public JComponent createComponent() {
        executable = new JTextField(40);
        JLabel label = new JLabel("RFL executable:");
        label.setLabelFor(executable);
        JPanel field = new JPanel(new BorderLayout(8, 0));
        field.add(label, BorderLayout.WEST);
        field.add(executable, BorderLayout.CENTER);
        JPanel content = new JPanel(new BorderLayout(0, 8));
        content.add(field, BorderLayout.NORTH);
        content.add(new JLabel("<html>Leave empty to find rfl on PATH, then target/release/rfl, then target/debug/rfl.<br>"
            + "The project directory and its parents are searched. Relative paths start at the project directory.<br>"
            + "Select the executable only; the plugin adds the lsp argument.<br>"
            + "Applying a change restarts the language server.</html>"), BorderLayout.CENTER);
        JPanel panel = new JPanel(new BorderLayout());
        panel.add(content, BorderLayout.NORTH);
        reset();
        return panel;
    }

    @Override public boolean isModified() {
        return executable != null && !executable.getText().strip().equals(RflSettings.getInstance(project).getServerPath());
    }

    @Override public void apply() throws ConfigurationException {
        if (executable == null || !isModified()) return;
        String value = executable.getText().strip();
        // An empty setting enables auto-discovery, including after a future build.
        if (!value.isEmpty()) {
            try {
                ServerExecutable.resolve(value, project.getBasePath() == null ? null : Path.of(project.getBasePath()),
                    EnvironmentUtil.getValue("PATH"));
            } catch (IOException e) {
                throw new ConfigurationException(e.getMessage());
            }
        }
        RflSettings.getInstance(project).setServerPath(value);
        LspClientManager.getInstance(project).stopAndRestartClientsIfNeeded(RflLspSupportProvider.class);
    }

    @Override public void reset() {
        if (executable != null) executable.setText(RflSettings.getInstance(project).getServerPath());
    }

    @Override public void disposeUIResources() { executable = null; }
}
