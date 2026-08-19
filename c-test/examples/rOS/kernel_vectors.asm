; Must be the first object in KERNEL.SYS.

k_mount_vec:
    JAL R0, R0, fat16_mount
k_lookup_vec:
    JAL R0, R0, fat16_lookup
k_resolve_vec:
    JAL R0, R0, fat16_resolve
k_dir_open_vec:
    JAL R0, R0, fat16_dir_open
k_dir_next_vec:
    JAL R0, R0, fat16_dir_next
k_read_at_vec:
    JAL R0, R0, fat16_read_at
k_exec_vec:
    JAL R0, R0, k_exec
k_exit_vec:
    JAL R0, R0, k_exit
