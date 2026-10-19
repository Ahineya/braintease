import assert from 'node:assert/strict';
import test from 'node:test';
import {createHash} from 'node:crypto';
import {fat16Image} from '../../../c-test/examples/rOS/mkfat16.mjs';
import {rootImage} from '../../../ports/fuzix/tools/disk.mjs';
import {validateDisk,checkDiskSize,MAX_DISK_BYTES} from '../disk.mjs';

function fixture() {
  const kernel=Buffer.alloc(32);kernel.write('SYS1');
  for(const [at,n] of [[4,64000],[6,1],[8,3],[10,32]])kernel.writeUInt16LE(n,at);
  kernel.writeUInt32LE(1,16);
  return fat16Image([{name:'FUZIX.SYS',data:kernel},{name:'ROOTFS.IMG',data:rootImage([],4096,16)}]);
}
function locations(image) {
  const bps=image.readUInt16LE(11),fat=image.readUInt16LE(14)*bps,
    fatBytes=image.readUInt16LE(22)*bps,root=fat+image[16]*fatBytes,
    data=root+Math.ceil(image.readUInt16LE(17)*32/bps)*bps,clusterBytes=bps*image[13];
  return {fat,fatBytes,root,clusterBytes,file(index){const entry=root+index*32,
    cluster=image.readUInt16LE(entry+26);return {entry,cluster,at:data+(cluster-2)*clusterBytes};}};
}
test('valid exported layout, including typed-array offsets, stays unchanged',()=>{
  const image=fixture(),hash=x=>createHash('sha256').update(x).digest('hex'),before=hash(image);
  assert.deepEqual(validateDisk(image),{bytes:image.length,rootBytes:8*1024*1024});
  const padded=Buffer.concat([Buffer.alloc(7),image,Buffer.alloc(9)]);
  assert.equal(validateDisk(padded.subarray(7,-9)).bytes,image.length);
  assert.equal(hash(image),before);
});
test('empty, oversized, unrelated, and truncated files are rejected',()=>{
  assert.throws(()=>checkDiskSize(0));assert.throws(()=>checkDiskSize(MAX_DISK_BYTES+1),/256 MiB/);
  assert.throws(()=>validateDisk(Buffer.alloc(1024)),/FAT16/);
  assert.throws(()=>validateDisk(fixture().subarray(0,-512)),/incomplete/);
});
test('missing system file and conflicting FAT copies are rejected',()=>{
  let image=fixture(),l=locations(image);image[l.root]=0xe5;
  assert.throws(()=>validateDisk(image),/containing FUZIX/);
  image=fixture();l=locations(image);image[l.fat+l.fatBytes+10]^=1;
  assert.throws(()=>validateDisk(image),/disagree/);
});
test('cyclic and overlapping file chains are rejected',()=>{
  let image=fixture(),l=locations(image),f=l.file(1);
  for(let i=0;i<image[16];i++)image.writeUInt16LE(f.cluster,l.fat+i*l.fatBytes+f.cluster*2);
  assert.throws(()=>validateDisk(image),/broken or overlapping/);
  image=fixture();l=locations(image);image.writeUInt16LE(l.file(0).cluster,l.file(1).entry+26);
  assert.throws(()=>validateDisk(image),/overlapping/);
});
test('incompatible kernel and filesystem geometry are rejected',()=>{
  let image=fixture(),l=locations(image);image.writeUInt16LE(4096,l.file(0).at+4);
  assert.throws(()=>validateDisk(image),/kernel is incompatible/);
  image=fixture();l=locations(image);image[l.file(1).at+18]=50;
  assert.throws(()=>validateDisk(image),/C16 V3/);
  image=fixture();l=locations(image);image.writeUInt32LE(4097,l.file(1).at+2052);
  assert.throws(()=>validateDisk(image),/C16 V3/);
});
