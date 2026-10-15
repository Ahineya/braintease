package org.ripple.rfl;

import com.intellij.openapi.components.*;
import com.intellij.openapi.project.Project;
import org.jetbrains.annotations.NotNull;

@State(name = "RflSettings", storages = @Storage(StoragePathMacros.WORKSPACE_FILE))
public final class RflSettings implements PersistentStateComponent<RflSettings.Data> {
    public static final class Data {
        public String serverPath = "";
    }

    private Data data = new Data();

    public static RflSettings getInstance(Project project) { return project.getService(RflSettings.class); }
    @Override public @NotNull Data getState() { return data; }
    @Override public void loadState(@NotNull Data state) { data = state; }
    public String getServerPath() { return data.serverPath == null ? "" : data.serverPath; }
    public void setServerPath(String path) { data.serverPath = path; }
}
