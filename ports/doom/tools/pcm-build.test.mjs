import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
const port=path.dirname(path.dirname(fileURLToPath(import.meta.url)));
function config(directory,args,body) {
  return execFileSync('make',['-s','--no-print-directory','-C',directory,...args,'-f','Makefile','-f','-','pcm-config-check'],{encoding:'utf8',input:`pcm-config-check:\n\t@${body}\n`}).trim();
}
test('BF forces PCM and music off even if explicitly requested, and omits both modules',()=>{
  const result=config(port,['TARGET=bf','PCM_AUDIO=1','MUSIC_AUDIO=1'],`printf '%s|%s|%s|%s' '$(PCM_AUDIO):$(MUSIC_AUDIO)' '$(filter %pcm_sound.c %music_sound.c,$(RIPPLE_SOURCES))' '$(filter -DDOOM_PCM=%,$(RIPPLE_FLAGS))' '$(filter -DDOOM_MUSIC=%,$(RIPPLE_FLAGS))'`);
  assert.equal(result,'0:0||-DDOOM_PCM=0|-DDOOM_MUSIC=0');
});
test('normal Doom includes PCM and the BF runtime source set excludes audio helpers',()=>{
  assert.equal(config(port,['TARGET=ripple'],`printf '%s|%s' '$(PCM_AUDIO)' '$(filter %pcm_sound.c,$(RIPPLE_SOURCES))'`),'1|src/pcm_sound.c');
  assert.equal(config(path.join(port,'../../runtime'),['AUDIO=0'],`printf '%s' '$(filter vm_audio.c,$(C_SOURCES))'`),'');
  assert.equal(config(path.join(port,'../../runtime'),['AUDIO=0'],`printf '%s' '$(filter vm_music.c,$(C_SOURCES))'`),'');
});
