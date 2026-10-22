import fs from 'node:fs';
import {openImage} from '../../../ports/fuzix/tools/image.mjs';
import {rootReader} from '../../../ports/fuzix/tools/read-root.mjs';
const [disk,name,output]=process.argv.slice(2);
const data=rootReader(openImage(disk)).read(name);
if(data.some(value=>value>255))throw Error('Screenshot contains non-octet cells');
fs.writeFileSync(output,Buffer.from(data));
