// Read C16 files without materializing the disk or sparse file holes.
import {checkRoot} from './fsck.mjs';
import {CELLS, BLOCK} from './layout.mjs';
export function rootReader(image) {
  const checked = checkRoot(image), {nodes} = checked;
  function cells(node) {
    const result = new Uint16Array(node.size);
    for (const [index, block] of node.map) {
      const part = checked.image.subarray(block * BLOCK, (block + 1) * BLOCK);
      for (let i = 0; i < CELLS && index * CELLS + i < node.size; i++) result[index * CELLS + i] = part.readUInt16LE(i * 2);
    }
    return result;
  }
  function lookup(path) {
    let node = nodes.get(1);
    for (const name of path.split('/').filter(Boolean)) {
      const data = cells(node); let child;
      for (let i = 0; i < data.length; i += 32) {
        if (data[i] && String.fromCharCode(...data.subarray(i + 1, i + 31)).split('\0')[0] === name) { child = data[i]; break; }
      }
      if (!child) throw Error(`missing ${path}`);
      node = nodes.get(child);
    }
    return node;
  }
  return {checked, lookup, cells, read: path => cells(lookup(path))};
}
