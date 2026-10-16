# Run with: tcl /hello.tcl Ripple
set name Ripple
if {$argc > 0} {set name [lindex $argv 0]}
puts [exec cowsay "Hello, $name! Tcl is alive."]
proc square {n} {return [expr {$n * $n}]}
for {set n 1} {$n <= 5} {incr n} {
    puts "square($n) = [square $n]"
}
set f [open /tcl-visits a]
puts $f "Hello, $name!"
close $f
puts {Saved this visit in /tcl-visits.}
