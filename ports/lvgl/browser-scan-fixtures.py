"""Public synthetic directories for the non-mutating browser scanner checks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def prepare(base=None):
    """Return (fixture root, explicit guest manifest), without private inputs."""
    folder = Path(base) if base is not None else ROOT / 'build/lvgl/browser-scan-fixtures'
    folder = folder.resolve()
    small, limit = folder / 'small', folder / 'limit'
    for path in (folder, small, limit, small / 'Alpha', small / 'empty', small / 'zeta'):
        path.mkdir(parents=True, exist_ok=True)
    for name in ('A file.txt', 'A Україна.txt', 'café.txt', 'last.txt', '.secret', 'Alpha/nested.txt'):
        (small / name).write_text('Public LVGL browser scan fixture\n', encoding='utf-8')
    links = {
        'alias-rel': 'Alpha',
        'alias-abs': str(small / 'zeta'),
        'alias-up': '../limit',
        'loop-a': 'loop-b',
        'loop-b': 'loop-a',
        'dangling': 'missing',
    }
    for name, target in links.items():
        path = small / name
        if path.is_symlink():
            path.unlink()
        path.symlink_to(target)
    for index in range(1025):
        (limit / f'file-{index:04}.txt').write_text(f'Public fixture {index}\n', encoding='utf-8')

    def guest_name(path):
        # The image installer maps JS code units directly to C16 cells;
        # filenames and symlink contents must carry logical UTF-8 octets.
        return str(path).encode('utf-8').decode('latin-1')

    manifest = []
    for path in (folder, *sorted(folder.rglob('*'))):
        item = dict(name=guest_name(path))
        if path.is_symlink():
            item.update(mode=0o120777, text=guest_name(path.readlink()))
        elif path.is_dir():
            item.update(directory=True, mode=0o40755)
        else:
            item.update(path=str(path), mode=0o100644)
        manifest.append(item)
    return folder, manifest


if __name__ == '__main__':
    print(prepare()[0])
