# Guest-native build coordination. All compiler stages run as guest processes.
# build: one Ripple tool generation (also works in Fuse-X).
# bootstrap: three generations, comparison and execution checks (FUZIX/RVM).
proc sameContents {path text} {
    if [file exists $path] {
        set f [open $path r]
        set old [read $f]
        close $f
        return [string equal $old $text]
    }
    return 0
}
proc buildStage {source output seed sdk target} {
    file mkdir [file join $output bin] [file join $output obj]
    set flags {}
    catch {set flags $env(CFLAGS)}
    set config "seed=$seed\nsdk=$sdk\nflags=$flags\n"
    foreach path [list [file join $source source-id] [file join $sdk sdk-id] [file join $sdk seed-id]] {
        set f [open $path r]
        append config [read $f]
        close $f
    }
    foreach tool {cc rcc-c rcc-ripple rcc-as rcc-ld} {
        set path [file join $seed $tool]
        append config "[file mtime $path]:[file size $path]\n"
    }
    set stamp [file join $output inputs.stamp]
    if {[sameContents $stamp $config] == 0} {
        # C16 timestamps have one-second resolution; a changed configuration
        # must invalidate outputs even when it is edited within that second.
        foreach path [glob -nocomplain [file join $output obj *.rpo]] {file delete $path}
        foreach tool {cc rcc-c rcc-ripple rcc-as rcc-ld} {
            set path [file join $output bin $tool]
            if [file exists $path] {file delete $path}
        }
        # Record success only after invalidation, so interruption is retryable.
        set f [open $stamp w]
        puts -nonewline $f $config
        close $f
    }
    puts "Building $target in $output using $seed"
    exec make -r -f Makefile "BUILD=$output" "CC=$seed/cc" "TOOLS=$seed" "SDK=$sdk" $target >@stdout 2>@stderr
}
proc checkStage {source output tools sdk} {
    puts "Checking programs with $tools"
    foreach test {toolchain sdk_null} {
        set program [file join $output $test]
        puts "Compiling $test"
        exec [file join $tools cc] -v --tools $tools --sdk $sdk [file join tests $test.c] -o $program >@stdout 2>@stderr
        puts "Running $test"
        exec $program >@stdout 2>@stderr
    }
}
proc main {arguments script} {
    set caller [pwd]
    cd [file dirname $script]
    set source [pwd]
    set mode build
    set count [llength $arguments]
    if [llength $arguments] {set mode [lindex $arguments 0]}
    set output [file join $source build]
    if {$count > 1} {set output [file join $caller [lindex $arguments 1]]}
    set seed /bin
    if {$count > 2} {set seed [file join $caller [lindex $arguments 2]]}
    set sdk /usr/lib/rcc
    if [catch {set sdk $env(RCC_SDK)}] {set sdk /usr/lib/rcc}
    if [string equal $mode build] {
        buildStage $source $output $seed $sdk all
        puts "Ripple compiler generation ready: $output/bin"
        return
    }
    if [string equal $mode tools] {
        buildStage $source $output $seed $sdk tools
        return
    }
    if [string equal $mode check] {
        checkStage $source $output [file join $output bin] $sdk
        return
    }
    if [string equal $mode bootstrap] {
        foreach generation {1 2 3} {
            set stage [file join $output stage$generation]
            buildStage $source $stage $seed $sdk all
            set seed [file join $stage bin]
            checkStage $source $stage $seed $sdk
        }
        foreach tool {cc rcc-c rcc-ripple rcc-as rcc-ld} {
            exec cmp [file join $output stage2 bin $tool] [file join $output stage3 bin $tool] >@stdout 2>@stderr
        }
        puts {Bootstrap passed: generations 2 and 3 match; programs run successfully.}
        return
    }
    error {Usage: tcl bootstrap.tcl build|tools|check|bootstrap [output-directory [seed-tools-directory]]}
}
if [catch {main $argv $argv0} message] {
    puts stderr "bootstrap: $message"
    exit 1
}
