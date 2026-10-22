// Graphite's curated application catalog. User pins are never overwritten.
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
const directory = new URL('./', import.meta.url);
export function graphiteCatalogFiles() {
    return fs.readdirSync(directory).filter(name => name.endsWith('.ini')).sort()
        .map(name => ({name: `/usr/share/applications/${name}`, mode: 0o100644,
            data: fs.readFileSync(fileURLToPath(new URL(name, directory)))}));
}
