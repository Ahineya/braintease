// Shared guest data for RFL Commander on both operating systems.
import fs from 'node:fs';

export function commanderFiles() {
  const source = new URL('../apps/commander/themes/', import.meta.url);
  return [
    ...['classic', 'graphite', 'mono'].map(name => ({
      name: `/usr/share/mc/themes/${name}.ini`, mode: 0o100644,
      data: fs.readFileSync(new URL(`${name}.ini`, source))
    })),
    {name: '/usr/share/mc/config.example.ini', mode: 0o100644,
      data: fs.readFileSync(new URL('mc.ini', source))},
    {name: '/etc/mc.ini', mode: 0o100644, preserve: true,
      data: fs.readFileSync(new URL('mc.ini', source))}
  ];
}

// Editor resources share paths and upgrade policy on both systems.
export function riftFiles() {
  const source = new URL('../apps/rift/', import.meta.url);
  const file = (name, relative, preserve = false) => ({name, mode: 0o100644,
    ...(preserve ? {preserve: true} : {}), data: fs.readFileSync(new URL(relative, source))});
  return [
    ...['turbo', 'graphite', 'mono'].map(name =>
      file(`/usr/share/rift/themes/${name}.ini`, `themes/${name}.ini`)),
    file('/usr/share/rift/config.example.ini', 'themes/config.ini'),
    file('/usr/share/rift/theme.example.ini', 'themes/theme.ini'),
    file('/usr/share/rift/hello.bf', 'hello.bf'),
    file('/etc/rift.ini', 'themes/config.ini', true),
    file('/etc/rift/theme.ini', 'themes/theme.ini', true)
  ];
}
