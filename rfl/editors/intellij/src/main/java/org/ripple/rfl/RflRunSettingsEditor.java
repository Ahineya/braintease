package org.ripple.rfl;

import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.ui.TextFieldWithBrowseButton;
import com.intellij.util.ui.FormBuilder;
import org.jetbrains.annotations.NotNull;

import javax.swing.*;

public final class RflRunSettingsEditor extends SettingsEditor<RflRunConfiguration> {
    private final TextFieldWithBrowseButton source = new TextFieldWithBrowseButton();
    private final TextFieldWithBrowseButton workingDirectory = new TextFieldWithBrowseButton();
    private final JTextField output = new JTextField();
    private final JComboBox<RflCommand.Mode> mode = new JComboBox<>(RflCommand.Mode.values());
    private final JComboBox<String> target = new JComboBox<>(new String[] {"ripple", "native"});
    private final JCheckBox jit = new JCheckBox("Use Ripple JIT");
    private final JCheckBox emitIr = new JCheckBox("Print IR in the Run window");
    private final JCheckBox terminal = new JCheckBox("Emulate terminal for program input");
    private final JPanel panel;

    RflRunSettingsEditor(Project project) {
        source.addBrowseFolderListener(project, FileChooserDescriptorFactory.createSingleFileDescriptor("rfl").withTitle("RFL Source File"));
        workingDirectory.addBrowseFolderListener(project, FileChooserDescriptorFactory.createSingleFolderDescriptor().withTitle("Working Directory"));
        output.setToolTipText("Optional. Relative to the working directory. Empty uses the compiler's default beside the source file.");
        workingDirectory.setToolTipText("Empty uses the project directory. Paths may use $PROJECT_DIR$.");
        terminal.setToolTipText("Provides a pseudo terminal. In the Run console, press Enter to send typed input to the program.");
        panel = FormBuilder.createFormBuilder()
            .addLabeledComponent("Source file:", source)
            .addLabeledComponent("Action:", mode)
            .addLabeledComponent("Target:", target)
            .addLabeledComponent("Working directory:", workingDirectory)
            .addLabeledComponent("Output file:", output)
            .addComponent(jit).addComponent(emitIr).addComponent(terminal)
            .addComponent(new JLabel("<html>Uses the executable in Languages &amp; Frameworks → RFL.<br>"
                + "Run compiles and launches the program. Compile writes assembly or a native executable.<br>"
                + "Check reports compiler diagnostics. Relative source paths start at the project directory.</html>"))
            .addComponentFillVertically(new JPanel(), 0).getPanel();
        mode.addActionListener(event -> updateEnabled());
        target.addActionListener(event -> updateEnabled());
        emitIr.addActionListener(event -> updateEnabled());
        updateEnabled();
    }

    private void updateEnabled() {
        boolean run = mode.getSelectedItem() == RflCommand.Mode.RUN;
        boolean compile = mode.getSelectedItem() == RflCommand.Mode.COMPILE;
        jit.setEnabled(run && "ripple".equals(target.getSelectedItem()));
        emitIr.setEnabled(compile);
        output.setEnabled(compile && !emitIr.isSelected());
        terminal.setEnabled(run);
    }

    @Override protected void resetEditorFrom(@NotNull RflRunConfiguration configuration) {
        RflCommand.Options options = configuration.commandOptions();
        source.setText(options.source);
        workingDirectory.setText(options.workingDirectory);
        output.setText(options.output);
        for (RflCommand.Mode value : RflCommand.Mode.values()) if (value.argument.equals(options.mode)) mode.setSelectedItem(value);
        target.setSelectedItem(options.target);
        jit.setSelected(options.jit);
        emitIr.setSelected(options.emitIr);
        terminal.setSelected(options.terminal);
        updateEnabled();
    }

    @Override protected void applyEditorTo(@NotNull RflRunConfiguration configuration) {
        RflCommand.Options options = new RflCommand.Options();
        options.source = source.getText().strip();
        options.workingDirectory = workingDirectory.getText().strip();
        options.mode = ((RflCommand.Mode) mode.getSelectedItem()).argument;
        options.target = (String) target.getSelectedItem();
        options.jit = jit.isEnabled() && jit.isSelected();
        options.emitIr = emitIr.isEnabled() && emitIr.isSelected();
        options.output = output.isEnabled() ? output.getText().strip() : "";
        options.terminal = terminal.isSelected();
        configuration.setCommandOptions(options);
    }

    @Override protected @NotNull JComponent createEditor() { return panel; }
}
