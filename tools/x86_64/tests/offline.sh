# Functional acceptance for the offline x64 collection, using a private directory.
set -e
work=`mktemp -d /tmp/x64-appcheck-XXXXXX`
test -n "$work"
cd "$work"
pwd
whoami > who
 grep root who
id
 groups
uname
 date
 uptime
free
ps
 df
stty -a
 tty
echo OFFLINE_SYSTEM_OK

echo 'beta
alpha
alpha' > data
sort data | uniq > sorted
echo 'alpha
beta' > expected
cmp sorted expected
test `wc -l < data` -eq 3
head -1 sorted > line
grep alpha line
tail -1 sorted > line
fgrep beta line
sed 's/alpha/omega/g' sorted > changed
grep omega changed
cut -c1 sorted > letters
echo 'a
b' > expected
cmp letters expected
tr '[a-z]' '[A-Z]' < sorted > upper
grep ALPHA upper
rev < line > reversed
grep ateb reversed
basename /bin/awk > name
grep awk name
dirname /bin/awk > name
grep /bin name
expr 20 + 22 > number
grep 42 number
seq 1 3 > numbers
test `wc -l < numbers` -eq 3
echo OFFLINE_TEXT_OK

cp sorted copy
cmp sorted copy
ln copy hard
mv copy moved
cmp hard moved
chmod 600 moved
chown root moved
chgrp root moved
touch touched
test -f touched
mkdir sub
rmdir sub
mkfifo fifo
rm fifo
dd if=sorted of=copy bs=7
cmp sorted copy
cksum copy
sum copy
od -c copy > octets
test -s octets
find . -name moved -print > found
grep moved found
du .
ls -l moved
rm touched
echo OFFLINE_FILES_OK

awk 'BEGIN { print 6*7; print sqrt(81); print sin(0); print 2^10 }' > numeric
echo '42
9
0
1024' > expected
cmp numeric expected
echo 'PRINT 6 * 7
END' > math.bas
basic math.bas > basic.out
grep 42 basic.out
echo 'puts [expr 6 * 7]' > math.tcl
picol math.tcl > tcl.out
grep 42 tcl.out
tcl math.tcl > tcl.out
grep 42 tcl.out
tclsh math.tcl > tcl.out
grep 42 tcl.out
echo 'define(answer,42)answer' > macros
m4 macros > expanded
grep 42 expanded
echo '6 7 * p' | dc > dc.out
grep 42 dc.out
echo OFFLINE_LANGUAGES_OK

echo 'all:
	echo MAKE_OK > made' > Makefile
make
grep MAKE_OK made
echo 'a
EDITOR_OK
.
w
q' > edits
touch editfile
ed editfile < edits
grep EDITOR_OK editfile
tar cf files.tar sorted
mkdir unpacked
cd unpacked
tar xf ../files.tar
cmp sorted ../sorted
cd ..
man awk > manual
test -s manual
which awk > where
grep /bin/awk where
printenv PATH > path
grep /bin path
env /bin/echo ENV_OK > env.out
grep ENV_OK env.out
nice /bin/echo NICE_OK > nice.out
grep NICE_OK nice.out
echo xargs-ok | xargs echo > args
grep xargs-ok args
time /bin/true
cd /
rm -r "$work"
sync
echo OFFLINE_APPS_OK
