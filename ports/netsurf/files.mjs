// Files installed with the graphical browser on both guest platforms.
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
const fixtures = new URL('./fixtures/', import.meta.url);
export function netsurfFiles() {
  return ['index.html','next.html','images.html',
    ...fs.readdirSync(new URL('images/', fixtures)).map(name=>'images/'+name)]
    .map(name=>({name:'/usr/share/netsurf/'+name,mode:0o100644,
      data:fs.readFileSync(fileURLToPath(new URL(name, fixtures)))}));
}
