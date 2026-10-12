# Game data

`doom1.wad` is the repository's existing Doom shareware IWAD, moved here from
`c-test/examples/doom/doom1.wad` without changing its contents.

- Serialized length: 4,196,020 octets
- SHA-256: `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`

The WAD is id Software game data, separate from the engine's source license.
Builds do not download game data. The launcher and tests stage a padded copy
before attaching it as a VM disk, preserving this source asset.

Use `WAD=/path/to/DOOM1.WAD` with Make or `--wad PATH` with `run.sh` to choose an
existing local IWAD. Automated asset expectations target the bundled file.
