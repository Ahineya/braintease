// Shared managed data for the normal app installers and the standalone tool.
import fs from 'node:fs';

export function lynxFiles() {
  const local = name => new URL(name, import.meta.url);
  return [
    {name:'/etc/lynx.cfg', mode:0o100644, preserve:true,
     data:Buffer.from(fs.readFileSync(local('fixtures/lynx.cfg'), 'utf8').replace(
       'file:///mnt/host/local.html', 'file:///usr/share/lynx/start.html'))},
    {name:'/usr/share/lynx/start.html', mode:0o100644, data:fs.readFileSync(local('fixtures/start.html'))},
    {name:'/usr/share/lynx/COPYING', mode:0o100644, data:fs.readFileSync(local('upstream/COPYING'))},
  ];
}
