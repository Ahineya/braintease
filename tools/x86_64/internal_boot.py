#!/usr/bin/env python3
"""Stage/verify an initial Riplix EFI trial in an explicitly selected directory.

No device discovery, mounting, firmware entries or boot-order changes. Physical
ESP identity and the known-good image's hardware acceptance are caller inputs.
"""
import argparse
import ctypes
import datetime
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import sys
import tempfile

SCHEMA = 'riplix-internal-efi-stage'
DIRECTORY = 'Riplix'
STAGING_PREFIX = '.Riplix-stage-'
CANDIDATE = 'BOOTX64.EFI'
KNOWN_GOOD = 'BOOTGOOD.EFI'
MANIFEST = 'manifest.json'
MAX_IMAGE_BYTES = 256 * 1024 * 1024
MAX_MANIFEST_BYTES = 65536


class StageError(RuntimeError):
    pass


def expected_hash(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-fA-F]{64}', value):
        raise StageError('expected an independently supplied 64-digit SHA256')
    return value.lower()


def checked_directory_name(value):
    if not isinstance(value, str) or not re.fullmatch(r'Riplix(?:-[A-Za-z0-9][A-Za-z0-9-]{0,47})?', value):
        raise StageError('EFI directory must be Riplix or Riplix- followed by 1 to 48 ASCII letters, digits or hyphens')
    return value


def staging_prefix(directory_name):
    return f'.{checked_directory_name(directory_name)}-stage-'


def esp_identity(value):
    if not isinstance(value, str) or not value or len(value) > 256 or any(ord(c) < 32 for c in value):
        raise StageError('expected a nonempty caller-verified ESP identity, normally PARTUUID=...')
    return value


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_regular(path, limit):
    """Never follow a final symlink, read a device, or accept an unbounded file."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
            raise StageError(f'not a bounded regular file: {path}')
        result = bytearray()
        while len(result) <= limit:
            block = os.read(fd, min(1024 * 1024, limit + 1 - len(result)))
            if not block:
                break
            result.extend(block)
        after = os.fstat(fd)
        if len(result) > limit or len(result) != before.st_size or \
                (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != \
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise StageError(f'file changed while being read: {path}')
        return bytes(result)
    finally:
        os.close(fd)


def validate_efi(data):
    """Check a file-backed executable entry in an AMD64 PE32+ EFI application.

    This validates structure, not authenticity or successful HP booting. Hashes
    must come from separately accepted build/test evidence.
    """
    if len(data) < 64 or data[:2] != b'MZ':
        raise StageError('EFI image has no complete MZ header')
    pe = struct.unpack_from('<I', data, 60)[0]
    if pe < 64 or pe > len(data) - 24 or data[pe:pe + 4] != b'PE\0\0':
        raise StageError('EFI image has no bounded PE signature/header')
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    optional_size, characteristics = struct.unpack_from('<HH', data, pe + 20)
    optional = pe + 24
    table = optional + optional_size
    if machine != 0x8664 or not 1 <= sections <= 96 or not characteristics & 2:
        raise StageError('EFI image is not an AMD64 executable')
    if optional_size < 112 or table > len(data) or sections * 40 > len(data) - table:
        raise StageError('EFI image has truncated optional/section headers')
    if struct.unpack_from('<H', data, optional)[0] != 0x20b or \
            struct.unpack_from('<H', data, optional + 68)[0] != 10:
        raise StageError('EFI image must be PE32+ with EFI application subsystem 10')
    entry = struct.unpack_from('<I', data, optional + 16)[0]
    image_size, headers = struct.unpack_from('<II', data, optional + 56)
    if not entry or entry >= image_size or headers < table + sections * 40 or headers > len(data):
        raise StageError('EFI image has invalid entry, image size or header bounds')
    executable_entry = False
    for index in range(sections):
        at = table + index * 40
        virtual_size, address, raw_size, offset = struct.unpack_from('<IIII', data, at + 8)
        flags = struct.unpack_from('<I', data, at + 36)[0]
        if raw_size and (offset < headers or offset > len(data) or raw_size > len(data) - offset):
            raise StageError('EFI section extends outside file-backed image data')
        if address > image_size or max(virtual_size, raw_size) > image_size - address:
            raise StageError('EFI section extends outside virtual image bounds')
        if flags & 0x20000000 and address <= entry < address + raw_size:
            executable_entry = True
    if not executable_entry:
        raise StageError('EFI entry point is not backed by executable file data')
    return dict(machine='AMD64', optional_header='PE32+', subsystem='EFI_APPLICATION',
                entry_rva=entry, image_bytes=image_size, sections=sections)


def image_record(data, filename, directory_name=DIRECTORY):
    directory_name = checked_directory_name(directory_name)
    return dict(path=f'EFI/{directory_name}/{filename}', bytes=len(data), sha256=sha256(data), pe=validate_efi(data))


def checked_image(path, digest):
    data = read_regular(path, MAX_IMAGE_BYTES)
    if sha256(data) != digest:
        raise StageError(f'EFI source SHA256 mismatch: {path}')
    validate_efi(data)
    return data


def directory(path):
    if not stat.S_ISDIR(path.lstat().st_mode):
        raise StageError(f'expected a real directory, not a link: {path}')


def layout(root, directory_name=DIRECTORY):
    directory_name = checked_directory_name(directory_name)
    root = Path(root).absolute()
    directory(root)
    root = root.resolve(strict=True)
    entries = [p.name for p in root.iterdir() if p.name.casefold() == 'efi']
    if entries != ['EFI']:
        raise StageError('selected directory must contain exactly one canonical EFI directory')
    efi = root / 'EFI'
    directory(efi)
    if efi.stat().st_dev != root.stat().st_dev:
        raise StageError('EFI directory is on a different mounted filesystem')
    names = [p.name for p in efi.iterdir() if p.name.casefold() == directory_name.casefold()]
    if names and names != [directory_name]:
        raise StageError(f'unexpected or case-colliding EFI/{directory_name} destination')
    prefix = staging_prefix(directory_name)
    pending = sorted(p.name for p in efi.iterdir() if p.name.casefold().startswith(prefix.casefold()))
    return root, efi, efi / directory_name, pending


def decode_manifest(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise StageError(f'duplicate manifest key: {key}')
            result[key] = value
        return result
    try:
        return json.loads(data, object_pairs_hook=unique)
    except (UnicodeError, ValueError) as error:
        raise StageError(f'invalid manifest JSON: {error}') from None


def verify_generation(destination, candidate_hash, good_hash, identity, manifest_hash=None, directory_name=DIRECTORY):
    directory_name = checked_directory_name(directory_name)
    directory(destination)
    if {p.name for p in destination.iterdir()} != {CANDIDATE, KNOWN_GOOD, MANIFEST}:
        raise StageError(f'EFI/{directory_name} contains unexpected, missing or interrupted files; refusing to change it')
    raw = read_regular(destination / MANIFEST, MAX_MANIFEST_BYTES)
    if manifest_hash is not None and sha256(raw) != expected_hash(manifest_hash):
        raise StageError('EFI stage manifest SHA256 mismatch')
    manifest = decode_manifest(raw)
    if not isinstance(manifest, dict) or set(manifest) != {
            'schema', 'version', 'state', 'created_utc', 'caller_verified_esp_identity', 'files'} or \
            manifest.get('schema') != SCHEMA or type(manifest.get('version')) is not int or manifest['version'] != 1 or \
            manifest.get('state') != 'staged-not-boot-validated' or \
            manifest.get('caller_verified_esp_identity') != identity or \
            not isinstance(manifest.get('created_utc'), str) or \
            not isinstance(manifest.get('files'), dict) or set(manifest['files']) != {'candidate', 'known_good'}:
        raise StageError('unrecognized, rejected or mismatched EFI stage manifest')
    for role, filename, expected in [('candidate', CANDIDATE, candidate_hash), ('known_good', KNOWN_GOOD, good_hash)]:
        data = checked_image(destination / filename, expected)
        if manifest['files'][role] != image_record(data, filename, directory_name):
            raise StageError(f'EFI stage manifest does not describe exact {role} bytes')
    return dict(manifest_sha256=sha256(raw), manifest=manifest)


def inspect(root, candidate_hash, good_hash, identity, manifest_hash=None, directory_name=DIRECTORY):
    directory_name = checked_directory_name(directory_name)
    candidate_hash, good_hash = expected_hash(candidate_hash), expected_hash(good_hash)
    identity = esp_identity(identity)
    if candidate_hash == good_hash:
        raise StageError('candidate and known-good fallback must be distinct images')
    resolved, _, destination, pending = layout(root, directory_name)
    if not destination.exists() and not destination.is_symlink():
        return dict(state='not-staged', esp_directory=str(resolved), staging_directories=pending,
                    firmware_changed=False)
    verified = verify_generation(destination, candidate_hash, good_hash, identity, manifest_hash, directory_name)
    return dict(state='staged-not-boot-validated', esp_directory=str(resolved),
                **verified, staging_directories=pending,
                firmware_changed=False)


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def sync_regular(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise StageError(f'cannot synchronize a nonregular EFI stage file: {path}')
        os.fsync(fd)
    finally:
        os.close(fd)


def write_file(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    try:
        remaining = memoryview(data)
        while remaining:
            count = os.write(fd, remaining)
            if count <= 0:
                raise OSError(errno.EIO, 'short EFI stage write')
            remaining = remaining[count:]
        os.fsync(fd)
    finally:
        os.close(fd)
    if read_regular(path, len(data)) != data:
        raise StageError(f'EFI stage file failed readback: {path.name}')


def publish_directory(source, destination):
    """Atomically publish the entire generation without replacing any entry."""
    library = ctypes.CDLL(None, use_errno=True)
    if sys.platform == 'darwin':
        rename = library.renamex_np
        rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        arguments = (os.fsencode(source), os.fsencode(destination), 4)  # RENAME_EXCL, SDK sys/stdio.h.
    elif sys.platform.startswith('linux') and hasattr(library, 'renameat2'):
        rename = library.renameat2
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        arguments = (-100, os.fsencode(source), -100, os.fsencode(destination), 1)  # AT_FDCWD, RENAME_NOREPLACE.
    else:
        raise StageError('atomic no-replace directory publication is unavailable on this host')
    rename.restype = ctypes.c_int
    if rename(*arguments):
        number = ctypes.get_errno()
        raise OSError(number, os.strerror(number), str(destination))


def require_space(path, sizes):
    space = os.statvfs(path)
    block = max(space.f_frsize, 4096)
    required = sum((size + block - 1) // block * block for size in sizes) + 8 * block
    if space.f_bavail * space.f_frsize < required:
        raise StageError(f'insufficient ESP free space: need at least {required} bytes for complete staging')


def apply(root, candidate, candidate_hash, known_good, good_hash, identity, directory_name=DIRECTORY):
    directory_name = checked_directory_name(directory_name)
    candidate_hash, good_hash = expected_hash(candidate_hash), expected_hash(good_hash)
    identity = esp_identity(identity)
    if candidate_hash == good_hash:
        raise StageError('candidate and known-good fallback must be distinct images')
    # Reject invalid/corrupt source bytes before making any ESP changes. Retain
    # these immutable byte snapshots so a concurrent source edit cannot race copying.
    candidate_data = checked_image(candidate, candidate_hash)
    good_data = checked_image(known_good, good_hash)
    resolved, efi, destination, pending = layout(root, directory_name)
    if pending:
        raise StageError(f'prior interrupted/concurrent staging exists; inspect before proceeding: {pending}')
    if destination.exists() or destination.is_symlink():
        result = inspect(resolved, candidate_hash, good_hash, identity, directory_name=directory_name)
        if result['state'] != 'staged-not-boot-validated':
            raise StageError(f'existing EFI/{directory_name} disappeared during verification')
        # A preceding invocation might have published successfully but failed
        # its final fsync. A retry must finish durability checks, not merely
        # turn a readback into a false claim that publication was synchronized.
        for filename in (CANDIDATE, KNOWN_GOOD, MANIFEST):
            sync_regular(destination / filename)
        sync_directory(destination)
        sync_directory(efi)
        result = inspect(resolved, candidate_hash, good_hash, identity, result['manifest_sha256'], directory_name)
        result['action'] = 'already-staged-verified'
        return result
    manifest = dict(schema=SCHEMA, version=1, state='staged-not-boot-validated',
                    created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    caller_verified_esp_identity=identity,
                    files=dict(candidate=image_record(candidate_data, CANDIDATE, directory_name),
                               known_good=image_record(good_data, KNOWN_GOOD, directory_name)))
    manifest_data = (json.dumps(manifest, sort_keys=True, indent=2) + '\n').encode('utf-8')
    require_space(efi, [len(candidate_data), len(good_data), len(manifest_data)])
    staging = Path(tempfile.mkdtemp(prefix=staging_prefix(directory_name), dir=efi))
    try:
        write_file(staging / CANDIDATE, candidate_data)
        write_file(staging / KNOWN_GOOD, good_data)
        write_file(staging / MANIFEST, manifest_data)
        verify_generation(staging, candidate_hash, good_hash, identity, sha256(manifest_data), directory_name)
        sync_directory(staging)
        sync_directory(efi)
        publish_directory(staging, destination)
        sync_directory(efi)
        result = inspect(resolved, candidate_hash, good_hash, identity, sha256(manifest_data), directory_name)
        result['action'] = 'staged-and-verified'
        return result
    except Exception:
        # Only remove the uniquely named directory created by this invocation.
        # After publication it no longer exists; preserve the complete final
        # destination if a later fsync/readback fails. Abrupt process interruption
        # deliberately leaves a staging directory for explicit inspection.
        if staging.exists():
            shutil.rmtree(staging)
            sync_directory(efi)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('inspect', 'apply'):
        command = commands.add_parser(name)
        command.add_argument('--esp-dir', type=Path, required=True, help='explicit, already mounted ESP directory')
        command.add_argument('--esp-id', required=True, help='caller-verified partition identity, normally PARTUUID=...')
        command.add_argument('--directory', default=DIRECTORY, help='initial-stage EFI namespace, default Riplix; or Riplix-NAME')
        command.add_argument('--candidate-sha256', required=True)
        command.add_argument('--known-good-sha256', required=True)
        if name == 'apply':
            command.add_argument('--candidate', type=Path, required=True)
            command.add_argument('--known-good', type=Path, required=True)
        else:
            command.add_argument('--manifest-sha256')
    args = parser.parse_args(argv)
    try:
        if args.command == 'apply':
            result = apply(args.esp_dir, args.candidate, args.candidate_sha256,
                           args.known_good, args.known_good_sha256, args.esp_id, args.directory)
        else:
            result = inspect(args.esp_dir, args.candidate_sha256, args.known_good_sha256,
                             args.esp_id, args.manifest_sha256, args.directory)
        print(json.dumps(result, indent=2, sort_keys=True))
    except (StageError, OSError) as error:
        print(json.dumps(dict(error=str(error), firmware_changed=False)), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
