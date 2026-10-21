// Optional package: one engine executable plus guest-side aliases.
import fs from 'node:fs';
export const jpipeAliases=['jlines','jobj','jmap','jfilter','jreduce','jsort','jtext'];
export function jpipeFiles(binary) {
  return [
    {name:'/bin/jpipe',mode:0o100755,data:fs.readFileSync(binary)},
    ...jpipeAliases.map(name=>({name:'/bin/'+name,symlink:'jpipe'})),
    {name:'/usr/share/doc/jpipe/README.md',mode:0o100644,
     data:fs.readFileSync(new URL('./README.md',import.meta.url))}
  ];
}
