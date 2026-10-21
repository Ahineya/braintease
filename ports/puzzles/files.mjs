import fs from 'node:fs';
import path from 'node:path';
export const puzzleNames = JSON.parse(fs.readFileSync(new URL('catalog.json',import.meta.url),'utf8')).map(game=>game.id);
// One-time retirement of the former automatic /bin installation. The lazy
// callbacks read nothing once those paths are gone. Shared games and saves
// are outside these paths and are never changed by the OS installer.
export function retireInstalledPuzzles(appDirectory) {
  return ['puzzles', ...puzzleNames].map(name=>({
    name:`/bin/${name}`,
    removeIfMatches:()=>{
      const source=path.join(appDirectory,`${name}.fxe`);
      return fs.existsSync(source) ? fs.readFileSync(source) : null;
    },
  }));
}
