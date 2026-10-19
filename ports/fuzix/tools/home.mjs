// Root's personal files are migrated by directory-entry moves, retaining their
// inodes, permissions and native C16 contents. Never collect secrets on the host.
export const ROOT_HOME = '/root';

export function rootHomeFiles() {
  const personal = ['.shrc', '.profile', '.exrc', '.riftrc', '.muxrc', '.mux',
    '.musicrc', '.chat', '.config', '.local', '.cache', '.netsurf-cache',
    '.nxtetris-score', '.sokoban', '.lynxrc', '.lynx_cookies', 'lynx_bookmarks.html'];
  return [
    {name: ROOT_HOME, directory: true, mode: 0o40700},
    {name: '/etc/passwd', mode: 0o100644,
      data: Buffer.from('root:x:0:0:root:/root:/bin/sh\n'),
      update(old) {
        // Keep every other account/field, including a deliberately custom home.
        return old.map(c => String.fromCharCode(c)).join('').replace(
          /^(root:[^:\n]*:0:[^:\n]*:[^:\n]*:)\/:/m, '$1/root:')
          .split('').map(c => c.charCodeAt(0));
      }},
    ...personal.map(name => ({name: `${ROOT_HOME}/${name}`, moveFrom: `/${name}`})),
    {name: `${ROOT_HOME}/.telegram.conf`, moveFrom: '/etc/telegram.conf'},
    {name: `${ROOT_HOME}/.telegram.session`, moveFrom: '/telegram.session'},
  ];
}
