// Public browser-image allowlist. Never discover applications from a personal
// disk or install arbitrary files found in a build directory.
export const applications = `
init sh rift rfl-fm mux ptysh
basename dirname cat cp ls wc head tail grep fgrep sort uniq echo mkdir rmdir
touch tee cmp chmod cut colrm expand pwd rm sync true false seq sleep kill tr
od sum yes which env printenv chown chgrp du df sed ed xargs id whoami uname
date mkfifo mknod comm rev test ps vile levee find dd diff diffh diff3 tar
cowsay picol more less mktemp groups logname nohup fmt column cksum stty join
split tsort tty nice time clear pr col man wump hamurabi expr awk make m4 dc
free uptime sok top bf basic gterm nxdraw nano-X nanowm nxlaunch nxterm startx
nxeyes nxclock nxroach nxdsktop nxmsg nxselect nxcalc nxtetris
`.trim().split(/\s+/);

export const aliases = {
  mc: 'rfl-fm', vi: 'rift', mv: 'cp', ln: 'cp',
  tcl: 'picol', tclsh: 'picol', sokoban: 'sok',
};
