// SPDX-License-Identifier: GPL-2.0-or-later
import fs from 'node:fs';
import path from 'node:path';
export function fuseDEFiles(directory) {
    return [
        ['theme.fdi','/usr/share/fusede/themes/slate/theme.fdi'],
        ['theme.json','/usr/share/fusede/themes/slate/theme.json'],
        ['icons.fdi','/usr/share/fusede/icons/lucide/icons.fdi'],
        ['icons.json','/usr/share/fusede/icons/lucide/icons.json'],
        ['icons-LICENSE','/usr/share/fusede/icons/lucide/LICENSE'],
    ].map(([source,name])=>({name,mode:0o100644,data:fs.readFileSync(path.join(directory,source))}));
}
