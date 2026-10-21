"""Public, synthetic files shared by the host and disposable guest tests."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def prepare():
    folder = ROOT / 'build/lvgl/selector-fixtures'
    folder.mkdir(parents=True, exist_ok=True)
    for name in ('Alpha', 'empty', 'zeta'):
        (folder / name).mkdir(exist_ok=True)
    for name in ['A file.txt', 'A Україна.txt', 'café.txt', 'last.txt', '.secret', 'Alpha/nested.txt',
                 *[f'track-{i:03}.txt' for i in range(60)]]:
        (folder / name).write_text('LVGL selector test fixture\n')
    files = []
    for path in sorted(folder.rglob('*')):
        if path.is_dir():
            files.append(dict(name=str(path), directory=True, mode=0o40755))
        else:
            # The image installer writes JS code units directly into C16
            # directory cells. Explicitly supply logical UTF-8 octets, matching
            # the byte string created by guest applications and hostfs.
            files.append(dict(name=str(path).encode('utf-8').decode('latin-1'), path=str(path), mode=0o100644))
    return folder, files

if __name__ == '__main__': print(prepare()[0])
