# Common host, Ripple and hosted-native interpreter acceptance tests.
proc expect {ok message} {if {$ok == 0} {error $message}}
proc contents {path} {
    set f [open $path r]
    set result [read $f]
    close $f
    return $result
}
set base [lindex $argv 0]
file mkdir [file join $base a] [file join $base b] [file join $base {with spaces}]
cd $base
set words [list {one two} {semi;literal} {$not_substituted}]
set result [exec echo {*}$words]
expect [string equal $result {one two semi;literal $not_substituted}] {exec changed literal arguments}
expect [string equal [list {*}[list]] {}] {empty expansion failed}
set many [list]
for {set n 0} {$n < 200} {incr n} {lappend many $n}
expect [expr {[llength $many] == 200}] {large list failed}
set d [dict create compiler cc]
dict set d target ripple
expect [string equal [dict get $d target] ripple] {dictionary lookup failed}
expect [dict exists $d compiler] {dictionary membership failed}
set d [dict create a first b middle a last]
expect [string equal $d {a last b middle}] {dict create did not replace duplicate keys}
expect [expr {[dict size {a first b middle a last}] == 2}] {dictionary counted duplicate keys}
expect [string equal [dict get {a first a last} a] last] {dictionary did not retain the last value}
set env(RCC_BUILD_TEST) {literal value}
expect [string equal [exec sh -c {echo "$RCC_BUILD_TEST"}] {literal value}] {environment was not inherited}
foreach directory {a b} {
    set f [open [file join $directory source.c] w]
    puts $f {int main(void) { return 0; }}
    close $f
}
expect [string equal [glob */*.c] {a/source.c b/source.c}] {glob traversal failed}
expect [string equal [glob -nocomplain missing*] {}] {optional glob failed}
expect [file isfile a/source.c] {file metadata failed}
expect [file isdirectory {with spaces}] {space in directory name failed}
expect [string equal [file tail /a/b/] b] {file tail failed}
expect [string equal [file dirname /a/b] /a] {file dirname failed}
set code [catch {exec sh -c {exit 7}} message]
expect $code {exec accepted a failed process}
expect [string equal [lindex $errorCode 0] CHILDSTATUS] {missing child error category}
expect [string equal [lindex $errorCode 2] 7] {lost process exit status}
set longfile [lindex $argv 1]
expect [catch {exec cat $longfile}] {oversized capture was accepted}
exec cat $longfile >@stdout 2>@stderr
puts {TCL_BUILD_CHECKS_PASSED}
if {[llength $argv] > 2} {
    exec /bin/execargscheck >@stdout 2>@stderr
    cd [lindex $argv 2]
    exec make -r >@stdout 2>@stderr
    expect [string equal [contents result] "all|a|a b|default appended\n"] {make automatic variables or assignment failed}
    exec make -r VALUE=command >@stdout 2>@stderr
    expect [string equal [contents result] "all|a|a b|command\n"] {make command-line override failed}
    expect [catch {exec make -r broken}] {make accepted a failed command}
    expect [expr {![file exists broken]}] {make retained a failed output}
    set f [open preserve w]
    puts $f {keep existing output}
    close $f
    expect [catch {exec make -r preserve}] {make ignored a failed update}
    expect [string equal [contents preserve] "keep existing output\n"] {make deleted an unchanged output}
    puts {GUEST_BUILD_CHECKS_PASSED}
}
