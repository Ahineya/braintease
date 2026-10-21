#!/usr/bin/env python3
"""Build and boot the freestanding x86_64 kernel and RCC runtime in QEMU."""
import argparse
import hashlib
import ipaddress
import fcntl
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import stat
from contextlib import ExitStack

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
HEAP_PROFILE = 'bounded'


def wifi_probe_ipv4(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        raise argparse.ArgumentTypeError('expected a numeric unicast IPv4 target') from None
    if address.packed[0] == 0 or address.packed[0] >= 224 or address.is_loopback:
        raise argparse.ArgumentTypeError('expected a unicast IPv4 target')
    return str(address)


def executable(name, candidates=()):
    found = shutil.which(name)
    if found:
        return found
    for path in candidates:
        if Path(path).is_file() and os.access(path, os.X_OK):
            return str(path)
    raise RuntimeError(f'{name} not found; install it or add it to PATH')


def linker():
    # macOS's system linker emits Mach-O. UEFI needs a PE/COFF linker instead.
    try:
        return [executable('lld-link', [
            '/opt/homebrew/opt/lld/bin/lld-link',
            '/opt/homebrew/opt/lld@19/bin/lld-link',
            '/usr/local/opt/lld/bin/lld-link'])]
    except RuntimeError:
        rustc = shutil.which('rustc')
        if rustc:
            sysroot = Path(subprocess.check_output([rustc, '--print', 'sysroot'], text=True).strip())
            candidates = list((sysroot / 'lib/rustlib').glob('*/bin/rust-lld'))
            if len(candidates) == 1:
                return [str(candidates[0]), '-flavor', 'link']
        raise RuntimeError('lld-link not found; install LLVM LLD or a Rust toolchain') from None


def elf_linker():
    command = linker()
    if command[-1:] == ['link']:
        return [*command[:-1], 'gnu']
    path = Path(command[0]).with_name('ld.lld')
    return [executable('ld.lld', [path])]


def elf_symbols(path):
    """Return global definitions/imports to bind the explicit runtime surface."""
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise RuntimeError(f'expected little-endian ELF64: {path}')
    shoff = struct.unpack_from('<Q', data, 40)[0]
    size, count = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * size) for i in range(count)]
    defined, imports = set(), set()
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            name, info, _, index, _, _ = struct.unpack_from('<IBBHQQ', data, at)
            if info >> 4 not in (1, 2) or not name:
                continue
            symbol = names[name:names.index(0, name)].decode()
            (imports if index == 0 else defined).add(symbol)
    return defined, imports


def prepare_font(directory, kind, height, atlas):
    if kind == 'auto':
        kind = 'gterm' if sys.platform == 'darwin' else 'rom'
    if atlas is not None:
        data = atlas.read_bytes()
        kind = 'gterm'
    elif kind == 'gterm':
        path = directory / 'font.atlas'
        subprocess.run([str(ROOT / 'target/release/rcc-terminal-font'), str(height), str(path)], check=True)
        data = path.read_bytes()
    else:
        print('Boot console: ROM 8x16 font', flush=True)
        return None
    if len(data) < 16:
        raise RuntimeError('truncated gterm font atlas')
    magic, width, height, count = struct.unpack_from('<4sIII', data)
    if magic != b'GTF1' or not 1 <= width <= 64 or not 12 <= height <= 48 or count != 256 or \
            len(data) != 16 + width * height * count:
        raise RuntimeError('invalid gterm font atlas')
    path = directory / 'font.atlas'
    path.write_bytes(data)
    source = directory / 'font.S'
    source.write_text('.section .rodata\n.balign 4\n.globl font_payload_start,font_payload_end\n'
                      f'font_payload_start:\n.incbin "{path}"\nfont_payload_end:\n'
                      '.section .note.GNU-stack,"",@progbits\n')
    return source


def build(output, testing, stage, fault, font_scale, font, font_height, font_atlas, persistent=False,
          interrupt_controller='apic', high_framebuffer=False, framebuffer_cache='wc', storage='virtio', nvme_probe=False,
          root_identity=None, app_profile="minimal", network='none', wifi_probe=False, wifi_firmware=None, wifi_pnvm=None, wifi_scan=False, wifi_prepare_ssid=None, wifi_auth=False, wifi_assoc=False, wifi_psk_file=None, wifi_data_probe=None, network_config=None, system_services=False, root_mib=None, ssh_tools=None, ssh_authorized_key=None, root_source=None):
    clang = executable('clang')
    directory = output / (f'test-{fault}' if fault != 'none' else 'test' if testing else 'normal')
    esp = directory / 'esp'
    efi = esp / 'EFI/BOOT/BOOTX64.EFI'
    efi.parent.mkdir(parents=True, exist_ok=True)
    if wifi_scan and (wifi_firmware is None or wifi_pnvm is None):
        raise RuntimeError("Wi-Fi scan requires pinned firmware and PNVM")
    if wifi_prepare_ssid is not None:
        target = wifi_prepare_ssid.encode('utf-8')
        if not wifi_scan or not 1 <= len(target) <= 32:
            raise RuntimeError('Wi-Fi station setup requires --wifi-scan and a 1..32 octet SSID')
        # Numeric octets avoid C string escapes or command-line macro injection.
        (directory / 'wifi-target.h').write_text(
            'static const uint8_t wifi_target_ssid[] = {' + ','.join(map(str, target)) + '};\n')
    if wifi_auth and wifi_prepare_ssid is None:
        raise RuntimeError('Wi-Fi authentication requires --wifi-prepare-ssid')
    if wifi_assoc and not wifi_auth:
        raise RuntimeError("Wi-Fi association requires --wifi-auth")
    if wifi_psk_file is not None:
        if not wifi_assoc:
            raise RuntimeError('Wi-Fi PSK test requires --wifi-assoc')
        directory.chmod(0o700) # All generated objects/ELF/images contain the derived test key.
        with Path(wifi_psk_file).open('rb') as secret_file:
            password = secret_file.read(66).rstrip(b'\r\n')
        if not 8 <= len(password) <= 63 or any(c < 32 or c > 126 for c in password):
            raise RuntimeError('Wi-Fi test passphrase must be 8..63 printable ASCII octets')
        pmk = hashlib.pbkdf2_hmac('sha1', password, target, 4096, 32)
        password = None
        descriptor = os.open(directory / 'wifi-pmk.h', os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, 'w') as header_file:
            header_file.write('/* Private test credential-derived PMK; never commit. */\n'
                              'static const uint8_t wifi_test_pmk[32] = {' + ','.join(map(str, pmk)) + '};\n')
        pmk = None
    if wifi_data_probe is not None:
        if wifi_psk_file is None:
            raise RuntimeError('Wi-Fi data probe requires --wifi-psk-file')
        address = ipaddress.IPv4Address(wifi_probe_ipv4(wifi_data_probe))
        (directory / 'wifi-data-target.h').write_text(
            'static const uint8_t wifi_data_target[4] = {' + ','.join(map(str, address.packed)) + '};\n')
    if network == 'wifi' and (wifi_firmware is None or wifi_psk_file is None or nvme_probe or
                              (persistent and (storage != 'nvme' or root_identity is None))):
        raise RuntimeError('live Wi-Fi requires RAM root or exact-identity NVMe root and firmware/WPA2 profile')
    wifi_source = None
    if wifi_firmware is not None:
        if (persistent and (network != 'wifi' or storage != 'nvme' or root_identity is None)) or nvme_probe or network not in ('none', 'wifi') or stage not in ('runtime', 'riplix') or fault != 'none':
            raise RuntimeError('Wi-Fi firmware startup requires an isolated RAM-root runtime/riplix profile')
        from wifi_payload import prepare
        wifi_source = prepare(directory, wifi_firmware, wifi_pnvm)
    subprocess.run(['cargo', 'build', '--release', '-p', 'rcc-driver', '-p', 'rcc-native',
                    '-p', 'rcc-native-runtime'],
                   check=True, cwd=ROOT)
    font_source = prepare_font(directory, font, font_height, font_atlas)
    rcc, emitter = ROOT / 'target/release/rcc', ROOT / 'target/release/rcc-native-object'
    guest = directory / 'rcc.o'
    if stage in ('runtime', 'process'):
        ir = directory / 'program.ir'
        fixture = ROOT / 'c-test/tests/core/functions/test_freestanding_native.c' if stage == 'runtime' else HERE / 'tests/process.c'
        with ir.open('w') as stream:
            subprocess.run([str(rcc), 'compile', str(fixture),
                            '--target', 'native', '--emit-ir', '-I', str(ROOT / 'runtime/include')],
                           stdout=stream, check=True)
        inputs = [str(ir)]
    else:
        # Keep old hosted-profile IR out of the freestanding kernel build.
        native = output / f'riplix-ir-{HEAP_PROFILE}'
        print('Freestanding heap: bounded (banks 48..239)', flush=True)
        subprocess.run(['make', '-C', str(ROOT / 'fuse-x'), '-j4', f'BUILD={native}',
                        f'RCC={rcc}', f'FUSE_X_HEAP_PROFILE={HEAP_PROFILE}',
                        f'FREESTANDING_BOOTSTRAP={int(stage == "bootstrap")}', 'freestanding-ir'], check=True)
        inputs = [part for name in (native / 'freestanding-kernel-inputs.txt').read_text().splitlines()
                  for part in ('--kernel', name)]
        inputs += (native / 'freestanding-app-inputs.txt').read_text().splitlines()
    subprocess.run([str(emitter), '--triple', 'x86_64-unknown-none', *inputs, '-o', str(guest)], check=True)
    common = ['-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mno-red-zone',
              '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-unwind-tables',
              '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections']
    definitions = ['-DBOOT_TEST=1'] if testing else []
    if network != 'none':
        definitions += ['-DBOOT_NETWORK=1']
    if network == 'wifi':
        definitions += ['-DBOOT_WIFI_LIVE=1']
        if system_services:
            definitions += ['-DBOOT_WIFI_USERLAND=1']
    if wifi_probe:
        definitions += ['-DBOOT_WIFI_PROBE=1']
    if wifi_source:
        definitions += ['-DBOOT_WIFI_BRINGUP=1']
    if wifi_pnvm is not None:
        definitions += ['-DBOOT_WIFI_PNVM=1']
    if wifi_scan:
        definitions += ['-DBOOT_WIFI_SCAN=1']
    if wifi_prepare_ssid is not None:
        definitions += ['-DBOOT_WIFI_PREPARE=1', '-I', str(directory)]
    if wifi_auth:
        definitions += ['-DBOOT_WIFI_AUTH=1']
    if wifi_assoc:
        definitions += ['-DBOOT_WIFI_ASSOC=1']
    if wifi_psk_file is not None:
        definitions += ['-DBOOT_WIFI_WPA2=1']
    if wifi_data_probe is not None:
        definitions += ['-DBOOT_WIFI_DATA=1']
    if root_identity is not None:
        from partition_image import header
        (directory / 'root-identity.h').write_text(header(root_identity))
        definitions += ['-DBOOT_ROOT_PARTITION=1', '-I', str(directory)]
    if app_profile in ('offline', 'system'):
        definitions += ['-DBOOT_MATH=1']
    definitions += [f'-DCONSOLE_SCALE={font_scale}']
    if framebuffer_cache == 'uc':
        definitions += ['-DBOOT_FRAMEBUFFER_UC=1']
    if interrupt_controller == 'pic':
        definitions += ['-DBOOT_LEGACY_PIC=1']
    if high_framebuffer:
        definitions += ['-DBOOT_HIGH_FRAMEBUFFER_TEST=1']
    if stage == 'process':
        definitions += ['-DPROCESS_TEST=1']
    if font_source:
        definitions += ['-DBOOT_FONT_ATLAS=1']
    definitions += [f'-DBOOT_FAULT={dict(none=0, invalid_opcode=6, null_page=14, stack_overflow=8)[fault]}']
    if stage in ('riplix', 'bootstrap'):
        definitions += ['-DRIPLIX_BOOT=1']
    if nvme_probe:
        definitions += ['-DBOOT_NVME_PROBE=1']
    if persistent:
        definitions += ['-DPERSISTENT_ROOT=1']
        if storage == 'nvme':
            definitions += ['-DROOT_NVME=1']
    if stage == 'bootstrap':
        definitions += ['-DRIPLIX_BOOTSTRAP=1']
    vectors = directory / 'vectors.S'
    assembly = ['.text']
    errors = {8, 10, 11, 12, 13, 14, 17, 21, 29, 30}
    for vector in range(256):
        assembly += [f'isr_{vector}:']
        if vector not in errors:
            assembly += ['pushq $0']
        handler = 'irq_common' if 32 <= vector < 48 or vector == 255 else 'exception_common'
        assembly += [f'pushq ${vector}', f'jmp {handler}']
    assembly += ['.section .data.rel.ro', '.globl isr_vectors', 'isr_vectors:']
    assembly += [f'.quad isr_{vector}' for vector in range(256)]
    assembly += ['.section .note.GNU-stack,"",@progbits']
    vectors.write_text('\n'.join(assembly) + '\n')
    objects = [guest]
    if font_source:
        obj = directory / 'font.o'
        subprocess.run([clang, '--target=x86_64-unknown-none-elf', '-c', str(font_source), '-o', str(obj)], check=True)
        objects.append(obj)
    sources = [HERE / 'boot/arch.S', HERE / 'boot/kernel.c', HERE / 'boot/console.c', HERE / 'boot/input.c', HERE / 'boot/keyboard.c',
               HERE / 'boot/timer.c', HERE / 'boot/entropy.c', HERE / 'boot/acpi.c', HERE / 'boot/power.c', HERE / 'boot/runtime.c', vectors]
    if persistent or high_framebuffer or nvme_probe or network != 'none' or wifi_probe or wifi_source:
        sources += [HERE / 'boot/pci.c']
    if persistent or nvme_probe:
        sources += [HERE / ('boot/nvme.c' if nvme_probe or storage == 'nvme' else 'boot/block.c')]
    if nvme_probe or (persistent and storage == 'nvme') or network != 'none' or wifi_source:
        sources += [HERE / 'boot/dma.c']
    if network == 'user':
        sources += [HERE / 'boot/net.c']
    if wifi_probe:
        sources += [HERE / 'boot/wifi-probe.c']
    if wifi_source:
        sources += [HERE / ('boot/' + name) for name in ('wifi-image.c', 'wifi-pnvm.c', 'wifi-nvm.c', 'wifi-scan.c', 'wifi-security.c', 'wifi-prepare.c', 'wifi-auth.c', 'wifi-assoc.c', 'wifi-wpa.c', 'wifi-wpa-transport.c', 'wifi-ether.c', 'wifi-michael.c', 'wifi-live.c', 'wifi-rekey.c', 'wifi-data.c', 'wifi-crypto.c', 'wifi-sha1.c', 'wifi-aes.c', 'wifi-transport.c', 'wifi-restart.c', 'wifi-native.c')]
        sources.append(wifi_source)
    if nvme_probe or root_identity is not None:
        sources += [HERE / 'boot/gpt.c']
    if root_identity is not None:
        sources += [HERE / 'boot/root-window.c']
    if high_framebuffer:
        sources += [HERE / 'tests/highframebuffer.c']
    if testing:
        sources.append(HERE / 'tests/irq.S')
    for source in sources:
        obj = directory / (source.stem + '.o')
        subprocess.run([clang, '--target=x86_64-unknown-none-elf', *common, '-fpie', *definitions,
                        '-c', str(source), '-o', str(obj)], check=True)
        objects.append(obj)
    if stage in ('riplix', 'bootstrap'):
        root = directory / 'root.img'
        packages = []
        if stage == 'riplix':
            from apps import build_apps
            packages = [str(build_apps(output / f'userspace-{HEAP_PROFILE}', app_profile, network != 'none', system_services))]
            if network != 'none':
                packages += ['--network']
                if network == 'wifi':
                    packages += ['--wifi-test']
                if network_config is not None:
                    packages += ['--network-config', str(network_config.resolve())]
            if app_profile == 'offline':
                packages += ['--offline']
            if system_services:
                packages += ['--system-services']
            if ssh_tools is not None:
                packages += ['--ssh-tools', str(ssh_tools.resolve()), '--ssh-authorized-key', str(ssh_authorized_key.resolve())]
            if root_mib is not None:
                packages += ['--root-mib', str(root_mib)]
            if root_source is not None:
                packages += ['--upgrade', str(root_source.resolve())]
        subprocess.run(['node', str(HERE / 'riplix/root.mjs'), str(root), *packages], check=True)
        if not persistent:
            root_asm = directory / 'root.S'
            root_asm.write_text('.section .rodata\n.globl root_payload_start,root_payload_end\n'
                                f'root_payload_start:\n.incbin "{root}"\nroot_payload_end:\n'
                                '.section .note.GNU-stack,"",@progbits\n')
            obj = directory / 'root.o'
            subprocess.run([clang, '--target=x86_64-unknown-none-elf', '-c', str(root_asm), '-o', str(obj)], check=True)
            objects.append(obj)
    if app_profile in ('offline', 'system'):
        subprocess.run(['cargo', 'build', '--offline', '--release', '--manifest-path', str(HERE / 'math/Cargo.toml'),
                        '--target', 'x86_64-unknown-linux-gnu', '--target-dir', str(output / 'math')], check=True,
                       env=dict(os.environ, RUSTFLAGS='-C relocation-model=pic -C code-model=small -C no-redzone=yes '
                                '-C target-cpu=x86-64 -C target-feature=-avx,-avx2'))
        math_object = directory / 'math.o'
        subprocess.run([*elf_linker(), '-r', '--gc-sections',
                        '-u', 'rcc_native_ext___rcc_host_math', '-u', 'riplix_math_selftest', '--whole-archive',
                        str(output / 'math/x86_64-unknown-linux-gnu/release/libriplix_math.a'),
                        '--no-whole-archive', '-o', str(math_object)], check=True)
        objects.append(math_object)
    # Every as-yet unsupported ABI service is a named fatal trap. None can
    # silently succeed or fall through into host libc. Keep their inventory.
    defined, imports = set(), set()
    for obj in objects:
        definitions_in_object, imports_in_object = elf_symbols(obj)
        defined.update(definitions_in_object); imports.update(imports_in_object)
    defined.add("kernel_readonly_end")  # Defined by the reviewed kernel linker script.
    missing = sorted(imports - defined)
    if 'rcc_native_memview_v1' in imports:
        raise RuntimeError('checked freestanding build unexpectedly imports read-v1 memory views')
    if any(not name.startswith('rcc_native_') for name in missing):
        raise RuntimeError(f'unbound kernel symbols: {missing}')
    (directory / 'unsupported-services.json').write_text(json.dumps(missing, indent=2) + '\n')
    traps = directory / 'unsupported.S'
    assembly = ['.text']
    for index, name in enumerate(missing):
        assembly += [f'.globl {name}', f'{name}:', f'lea unsupported_{index}(%rip),%rdi',
                     'jmp unsupported_service']
    assembly += ['.section .rodata']
    for index, name in enumerate(missing):
        assembly += [f'unsupported_{index}: .asciz "unsupported service: {name}"']
    assembly += ['.section .note.GNU-stack,"",@progbits']
    traps.write_text('\n'.join(assembly) + '\n')
    obj = directory / 'unsupported.o'
    subprocess.run([clang, '--target=x86_64-unknown-none-elf', '-c', str(traps), '-o', str(obj)], check=True)
    objects.append(obj)
    kernel = directory / 'kernel.elf'
    subprocess.run([*elf_linker(), '-pie', '--no-dynamic-linker', '-Bsymbolic', '--no-undefined', '--gc-sections',
                    '-T', str(HERE / 'boot/kernel.ld'), '-o', str(kernel), *map(str, objects)], check=True)
    payload = directory / 'payload.S'
    payload.write_text('.section .rdata,"dr"\n.globl kernel_payload_start,kernel_payload_end\n'
                       f'kernel_payload_start:\n.incbin "{kernel}"\nkernel_payload_end:\n')
    payload_object = directory / 'payload.obj'
    subprocess.run([clang, '--target=x86_64-pc-windows-msvc', '-c', str(payload), '-o', str(payload_object)], check=True)
    obj = directory / 'loader.obj'
    subprocess.run([clang, '--target=x86_64-pc-windows-msvc', *common, '-mno-stack-arg-probe',
                    *(['-DBOOT_TEST=1'] if testing else []), '-c', str(HERE / 'boot/loader.c'), '-o', str(obj)], check=True)
    subprocess.run([*linker(), '/subsystem:efi_application', '/entry:efi_main',
                    '/nodefaultlib', '/machine:x64', '/dynamicbase', '/base:0x100000',
                    '/opt:ref', f'/out:{efi}', str(obj), str(payload_object)], check=True)
    data = efi.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0' or \
            struct.unpack_from('<H', data, pe + 4)[0] != 0x8664 or \
            struct.unpack_from('<H', data, pe + 24 + 68)[0] != 10:
        raise RuntimeError('output is not an x86_64 UEFI application')
    if not all(struct.unpack_from('<II', data, pe + 24 + 112 + 5 * 8)):
        raise RuntimeError('UEFI loader must carry a PE base relocation directory')
    print(f'Built {efi}', flush=True)
    (directory / 'build.json').write_text(json.dumps(dict(
        architecture='x86_64', stage=stage, app_profile=app_profile, system_services=system_services, root_mib=root_mib, network=network, wifi_probe=wifi_probe, wifi_bringup=wifi_source is not None, wifi_pnvm=wifi_pnvm is not None, wifi_scan=wifi_scan, wifi_station_setup=wifi_prepare_ssid is not None, wifi_auth=wifi_auth, wifi_assoc=wifi_assoc, wifi_wpa2=wifi_psk_file is not None, wifi_data_probe=wifi_data_probe,
        heap_profile=HEAP_PROFILE if stage in ('riplix', 'bootstrap') else None,
        storage=storage if persistent else 'ram',
        interrupt_controller=interrupt_controller, framebuffer_cache=framebuffer_cache,
        nvme_probe=nvme_probe, nvme_writes_disabled=nvme_probe,
        root_identity=root_identity,
    ), indent=2) + '\n')
    return esp


def validate_root_image(path):
    # Refuse device nodes, symlinks and the ordinary outer Fuse-X/FAT images.
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or not 16384 <= info.st_size <= 0x7fffffff or info.st_size % 2048:
        raise RuntimeError(f'expected a regular raw C16 V3 root image: {path}')
    with path.open('rb') as source:
        if source.read(19) != b'RIPPLE-FUZIX-C16-V3':
            raise RuntimeError(f'root image has an invalid C16 V3 signature: {path}')


def create_root_image(path, seed):
    """Initialize once, publishing a complete seed without replacing any file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        validate_root_image(path)
        return
    with tempfile.NamedTemporaryFile(prefix='.riplix-root-', dir=path.parent) as target:
        with seed.open('rb') as source:
            shutil.copyfileobj(source, target)
        target.flush(); os.fsync(target.fileno())
        validate_root_image(Path(target.name))
        try:
            os.link(target.name, path)
        except FileExistsError:
            pass
    validate_root_image(path)
    print(f'Persistent root: {path} (kept across runs and rebuilds)', flush=True)


def firmware(args):
    code = args.firmware_code or os.environ.get('OVMF_CODE')
    variables = args.firmware_vars or os.environ.get('OVMF_VARS')
    if code or variables:
        if not code or not variables:
            raise RuntimeError('provide both --firmware-code and --firmware-vars (or OVMF_CODE/OVMF_VARS)')
        pair = Path(code), Path(variables)
        if not all(path.is_file() for path in pair):
            raise RuntimeError('UEFI firmware files do not exist')
        return pair
    for directory, code_name, vars_name in [
        ('/opt/homebrew/share/qemu', 'edk2-x86_64-code.fd', 'edk2-i386-vars.fd'),
        ('/usr/local/share/qemu', 'edk2-x86_64-code.fd', 'edk2-i386-vars.fd'),
        ('/usr/share/OVMF', 'OVMF_CODE_4M.fd', 'OVMF_VARS_4M.fd'),
        ('/usr/share/OVMF', 'OVMF_CODE.fd', 'OVMF_VARS.fd'),
        ('/usr/share/edk2/ovmf', 'OVMF_CODE.fd', 'OVMF_VARS.fd'),
    ]:
        pair = Path(directory) / code_name, Path(directory) / vars_name
        if all(path.is_file() for path in pair):
            return pair
    raise RuntimeError('x86 UEFI firmware not found; install OVMF or set OVMF_CODE and OVMF_VARS')


def boot_disk(esp):
    """A disposable 64 MiB FAT16 superfloppy containing EFI/BOOT/BOOTX64.EFI.

    A real image avoids QEMU's host-directory FAT driver and needs no mtools,
    privileged mount, partition editor or access to a physical disk.
    """
    payload = (esp / 'EFI/BOOT/BOOTX64.EFI').read_bytes()
    image = bytearray(64 * 1024 * 1024)
    sector, cluster = 512, 2048
    fat_sectors, root_sector, data_sector = 128, 257, 289
    image[:11] = b'\xeb\x3c\x90RIPLIX  '
    struct.pack_into('<HBHBHHBHHHII', image, 11,
                     sector, 4, 1, 2, 512, 0, 0xf8, fat_sectors, 32, 64, 0, len(image) // sector)
    struct.pack_into('<BBBI', image, 36, 0x80, 0, 0x29, 0x5249504c)
    image[43:62] = b'RIPLIXBOOT FAT16   '
    image[510:512] = b'\x55\xaa'
    fat = bytearray(fat_sectors * sector)
    for index, value in [(0, 0xfff8), (1, 0xffff), (2, 0xffff), (3, 0xffff)]:
        struct.pack_into('<H', fat, index * 2, value)
    count = (len(payload) + cluster - 1) // cluster
    if not count or count + 4 > (len(image) - data_sector * sector) // cluster + 2:
        raise RuntimeError('UEFI image exceeds boot disk capacity')
    for index in range(4, 4 + count):
        struct.pack_into('<H', fat, index * 2, index + 1 if index < 3 + count else 0xffff)
    image[sector:sector + len(fat)] = fat
    image[(1 + fat_sectors) * sector:(1 + 2 * fat_sectors) * sector] = fat

    def entry(offset, name, attributes, first_cluster, size=0):
        image[offset:offset + 11] = name
        image[offset + 11] = attributes
        struct.pack_into('<HHHI', image, offset + 22, 0, 33, first_cluster, size)

    efi_directory = data_sector * sector
    boot_directory = efi_directory + cluster
    entry(root_sector * sector, b'EFI        ', 0x10, 2)
    entry(efi_directory, b'.          ', 0x10, 2)
    entry(efi_directory + 32, b'..         ', 0x10, 0)
    entry(efi_directory + 64, b'BOOT       ', 0x10, 3)
    entry(boot_directory, b'.          ', 0x10, 3)
    entry(boot_directory + 32, b'..         ', 0x10, 2)
    entry(boot_directory + 64, b'BOOTX64 EFI', 0x20, 4, len(payload))
    start = efi_directory + 2 * cluster
    image[start:start + len(payload)] = payload
    path = esp.parent / 'boot.img'
    path.write_bytes(image)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['build', 'test', 'run'])
    parser.add_argument('--output', type=Path, default=ROOT / 'build/x86_64-boot')
    parser.add_argument('--firmware-code', type=Path)
    parser.add_argument('--firmware-vars', type=Path)
    parser.add_argument('--timeout', type=int, default=180, help='boot test timeout in seconds')
    parser.add_argument('--debug', action='store_true', help='run paused with a GDB stub on localhost:1234')
    parser.add_argument('--stage', choices=['runtime', 'process', 'bootstrap', 'riplix'], default='riplix')
    parser.add_argument('--framebuffer-cache', choices=['wc', 'uc'], default='wc',
                        help='framebuffer PAT policy; device registers always stay uncached')
    parser.add_argument('--font-scale', type=int, choices=range(9), default=0,
                        help='integer framebuffer font scale: 0=automatic (default), 1-8=manual, limited to fit the display')
    parser.add_argument('--font', choices=['auto', 'gterm', 'rom'], default='auto',
                        help='auto uses native gterm on macOS, the ROM font elsewhere')
    parser.add_argument('--font-height', type=int, metavar='PIXELS', default=24,
                        help='gterm atlas cell height in pixels (default: 24)')
    parser.add_argument('--font-atlas', type=Path,
                        help='embed a previously exported gterm font.atlas on any build host')
    parser.add_argument('--fault', choices=['none', 'invalid_opcode', 'null_page', 'stack_overflow'], default='none')
    parser.add_argument('--apps', choices=['minimal', 'offline', 'system'], default='minimal',
                        help='small bootstrap set or offline CLI collection (requires persistent root)')
    parser.add_argument('--ssh-tools', type=Path, help='explicit x64 SSH executable directory')
    parser.add_argument('--ssh-authorized-key', type=Path, help='public development login key (never private key)')
    parser.add_argument('--system-services', action='store_true', help='install explicit x64 service/network boot profile and pkg bootstrap')
    parser.add_argument('--root-source', type=Path, help='regular existing C16 root to grow/install into a separate output')
    parser.add_argument('--root-mib', type=int, help='persistent C16 seed size, 16..1536 MiB')
    parser.add_argument('--ram-root', action='store_true', help='use the disposable embedded RAM root')
    parser.add_argument('--interrupt-controller', choices=['apic', 'pic'], default='apic',
                        help='local APIC timer (default), or explicit legacy PIC/PIT regression mode')
    parser.add_argument('--high-framebuffer', action='store_true',
                        help='test production console on a backed high physical PCI test BAR, then restore GOP')
    parser.add_argument('--usb-boot', action='store_true', help='GPT/FAT32 boot image; attach via emulated xHCI/USB')
    parser.add_argument('--accel', choices=['tcg', 'kvm'], default='tcg', help='QEMU accelerator (KVM requires Linux)')
    parser.add_argument('--cpu', help='QEMU CPU model; defaults to qemu64 for TCG, host for KVM')
    parser.add_argument('--memory', type=int, default=512, metavar='MiB',
                        help='VM RAM, 256..65536 MiB (default 512, also suitable for newer OVMF)')
    parser.add_argument('--storage', choices=['virtio', 'nvme'], default='virtio',
                        help='persistent virtual disk controller (raw C16 image by default; see --nvme-partition)')
    parser.add_argument('--nvme-partition', action='store_true',
                        help='bound native root writes to an explicitly identified dedicated GPT partition')
    parser.add_argument('--root-identity', type=Path,
                        help='version-1 disk/partition GUID and LBA identity JSON (build/run; tests use disposable identities)')
    parser.add_argument('--nvme-probe', action='store_true',
                        help='compile a read-only NVMe/GPT boot probe; requires --ram-root')
    parser.add_argument('--probe-expect', choices=['ok', 'gpt-error', 'missing'], default='ok',
                        help='test-only expected NVMe probe outcome')
    parser.add_argument('--probe-iommu', action='store_true',
                        help='add Intel VT-d to the QEMU probe test')
    parser.add_argument('--probe-disk', type=Path,
                        help='regular QEMU-only GPT image to probe read-only; never a physical device')
    parser.add_argument('--nvme-lba', type=int, choices=[512, 4096], default=512,
                        help='emulated NVMe namespace LBA size (default 512 bytes)')
    parser.add_argument('--disk', type=Path, help='persistent raw C16 V3 root image (run only; created once if missing)')
    parser.add_argument('--network', choices=['none', 'user', 'wifi'], default='none',
                        help='QEMU virtio user NAT, or experimental live HP Wi-Fi (RAM root/WPA2 required)')
    parser.add_argument('--network-config', type=Path, help='explicit IPv4/netmask/gateway/DNS JSON for the packaged manual network script')
    parser.add_argument('--telnet-port', type=int, default=2323,
                        help='localhost TCP port forwarded to guest Telnet (default 2323)')
    parser.add_argument('--wifi-probe', action='store_true',
                        help='read-only HP AX211 PCI/CSR discovery; no radio/firmware/DMA changes')
    parser.add_argument('--wifi-firmware', type=Path,
                        help='experimental HP AX211 firmware ALIVE test using pinned ucode; requires --ram-root')
    parser.add_argument('--wifi-pnvm', type=Path, help='matching pinned PNVM plus read-only firmware NVM probe; requires --wifi-firmware')
    parser.add_argument('--wifi-scan', action='store_true', help='bounded passive AX211 scan; requires --wifi-pnvm and --wifi-firmware')
    parser.add_argument('--wifi-prepare-ssid', help='select this SSID and test native station/queue setup; requires --wifi-scan; no authentication or keys')
    parser.add_argument('--wifi-auth', action='store_true', help='one bounded 802.11 authentication exchange with selected AP; requires --wifi-prepare-ssid; no association, password or keys')
    parser.add_argument("--wifi-assoc", action="store_true", help="bounded association and LINK/STA/MAC update; requires --wifi-auth; no WPA2 keys or IP")
    parser.add_argument("--wifi-data-probe", type=wifi_probe_ipv4, help="one encrypted ARP probe to this IPv4 target, no claimed IP; requires --wifi-psk-file")
    parser.add_argument("--wifi-psk-file", type=Path, help="private test passphrase file; requires --wifi-assoc; derived PMK is embedded in EFI")
    args = parser.parse_args()
    if os.environ.get('RCC_NATIVE_MEMORY', 'checked') != 'checked':
        parser.error('freestanding builds currently require RCC_NATIVE_MEMORY=checked; read-v1 support is pending')
    if not 12 <= args.font_height <= 48:
        parser.error('--font-height must be between 12 and 48 pixels')
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    if not 1024 <= args.telnet_port <= 65535:
        parser.error('--telnet-port must be between 1024 and 65535')
    if args.network_config and args.network == 'none':
        parser.error('--network-config requires --network')
    if args.network == 'wifi' and not args.network_config:
        parser.error('--network wifi requires an explicit --network-config')
    if args.network != 'none' and args.stage != 'riplix':
        parser.error('--network requires --stage riplix')
    if (args.network != 'none' or args.wifi_probe) and args.fault != 'none':
        parser.error('network/Wi-Fi discovery profiles require --fault none')
    if args.wifi_data_probe is not None and args.wifi_psk_file is None:
        parser.error('--wifi-data-probe requires --wifi-psk-file')
    if args.wifi_psk_file is not None and not args.wifi_assoc:
        parser.error("--wifi-psk-file requires --wifi-assoc")
    if args.wifi_assoc and not args.wifi_auth:
        parser.error("--wifi-assoc requires --wifi-auth")
    if args.wifi_auth and args.wifi_prepare_ssid is None:
        parser.error('--wifi-auth requires --wifi-prepare-ssid')
    if args.wifi_prepare_ssid is not None and (not args.wifi_scan or not 1 <= len(args.wifi_prepare_ssid.encode('utf-8')) <= 32):
        parser.error('--wifi-prepare-ssid requires --wifi-scan and a 1..32 octet SSID')
    if args.wifi_scan and not args.wifi_pnvm:
        parser.error('--wifi-scan requires --wifi-pnvm and --wifi-firmware')
    if args.wifi_pnvm and not args.wifi_firmware:
        parser.error('--wifi-pnvm requires --wifi-firmware')
    if args.network == 'wifi' and (not args.wifi_firmware or not args.wifi_psk_file or
                                    (not args.ram_root and not args.nvme_partition)):
        parser.error('--network wifi requires --ram-root or --nvme-partition, --wifi-firmware and --wifi-psk-file')
    if args.wifi_firmware and ((not args.ram_root and not (args.nvme_partition and args.network == 'wifi')) or args.stage not in ('runtime', 'riplix') or
                               args.nvme_probe or args.network not in ('none', 'wifi') or args.fault != 'none'):
        parser.error('--wifi-firmware requires RAM root or live exact-identity NVMe and stage runtime/riplix')
    if args.wifi_probe and args.stage not in ('riplix','runtime'):
        parser.error('--wifi-probe requires stage riplix or runtime')
    if not 256 <= args.memory <= 65536:
        parser.error('--memory must be between 256 and 65536 MiB')
    if args.debug and args.action != 'run':
        parser.error('--debug requires run')
    if args.fault != 'none' and args.action != 'test':
        parser.error('--fault requires test')
    if args.high_framebuffer and args.action != 'test':
        parser.error('--high-framebuffer is a test-only synthetic framebuffer profile')
    if args.font_atlas and args.font == 'rom':
        parser.error('--font-atlas cannot be combined with --font rom')
    if args.disk and (args.action != 'run' or args.ram_root or args.stage != 'riplix'):
        parser.error('--disk requires run --stage riplix without --ram-root')
    if args.nvme_partition and (args.storage != 'nvme' or args.ram_root or args.stage != 'riplix' or args.nvme_probe):
        parser.error('--nvme-partition requires --storage nvme --stage riplix without --ram-root/--nvme-probe')
    if args.root_identity and (not args.nvme_partition or args.action == 'test'):
        parser.error('--root-identity requires build/run --nvme-partition; tests always use disposable fixtures')
    if args.nvme_partition and args.action != 'test' and not args.root_identity:
        parser.error('build/run --nvme-partition needs an explicit --root-identity')
    if args.nvme_partition and args.action == 'run' and not args.disk:
        parser.error('run --nvme-partition needs an existing regular GPT --disk image')
    if args.apps in ('offline', 'system') and (args.ram_root or args.stage != 'riplix'):
        parser.error('--apps offline/system requires persistent --stage riplix')
    if args.root_source:
        if args.ram_root or args.stage != 'riplix' or args.action == 'test':
            parser.error('--root-source requires persistent build/run; tests always create disposable seeds')
        validate_root_image(args.root_source)
    if args.root_mib is not None and (not 16 <= args.root_mib <= 1536 or args.ram_root or args.stage != 'riplix'):
        parser.error('--root-mib requires persistent riplix and 16..1536 MiB')
    if bool(args.ssh_tools) != bool(args.ssh_authorized_key) or (args.ssh_tools and not args.system_services):
        parser.error('--ssh-tools and --ssh-authorized-key require each other and --system-services')
    if args.system_services and (args.ram_root or args.stage != 'riplix' or args.network == 'none'):
        parser.error('--system-services requires persistent riplix networking')
    root_identity = None
    if args.nvme_partition:
        from partition_image import identity, load_identity, validate_disk
        root_identity = identity(args.nvme_lba, max(16, (args.root_mib or (128 if args.apps == 'offline' else 16)) + 1) * 1024 * 1024) if args.action == 'test' else load_identity(args.root_identity)
        if root_identity['sector_bytes'] != args.nvme_lba:
            parser.error('--nvme-lba must match the root identity sector size')
        if args.disk:
            validate_disk(args.disk, args.nvme_lba)
    if args.nvme_probe and (not args.ram_root or args.stage not in ('riplix', 'runtime')):
        parser.error('--nvme-probe requires --ram-root and stage riplix or runtime')
    if (args.probe_expect != 'ok' or args.probe_iommu) and (not args.nvme_probe or args.action != 'test'):
        parser.error('--probe-expect/--probe-iommu require test --nvme-probe')
    if args.probe_expect == 'gpt-error' and not args.probe_disk:
        parser.error('--probe-expect gpt-error needs an explicit damaged --probe-disk fixture')
    if args.probe_expect == 'missing' and args.probe_disk:
        parser.error('--probe-expect missing cannot attach --probe-disk')
    if args.probe_disk and (not args.nvme_probe or args.action == 'build'):
        parser.error('--probe-disk requires test/run --nvme-probe')
    if args.probe_disk:
        from gpt_image import validate_probe_image
        validate_probe_image(args.probe_disk, args.nvme_lba)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    serial = output / 'serial.log'
    report = output / 'result.json'
    # Clear before compiling or resolving dependencies as either can fail.
    if args.action != 'build':
        report.unlink(missing_ok=True)
        serial.unlink(missing_ok=True)
        (output / 'network-result.json').unlink(missing_ok=True)
        (output / 'network-first-boot.pcap').unlink(missing_ok=True)
    testing = args.action == 'test'
    persistent = args.stage == 'riplix' and not args.ram_root
    esp = build(output, testing, args.stage, args.fault, args.font_scale,
                args.font, args.font_height, args.font_atlas, persistent, args.interrupt_controller,
                args.high_framebuffer, args.framebuffer_cache, args.storage, args.nvme_probe, root_identity, args.apps, args.network, args.wifi_probe, args.wifi_firmware, args.wifi_pnvm, args.wifi_scan, args.wifi_prepare_ssid, args.wifi_auth, args.wifi_assoc, args.wifi_psk_file, args.wifi_data_probe, args.network_config, args.system_services, args.root_mib, args.ssh_tools, args.ssh_authorized_key, args.root_source)
    if args.usb_boot:
        from usb_image import boot_disk as usb_boot_disk
        disk = usb_boot_disk(esp)
    else:
        disk = boot_disk(esp)
    if args.action == 'build':
        return
    qemu = executable('qemu-system-x86_64')
    code, variables = firmware(args)
    with tempfile.TemporaryDirectory(prefix='riplix-uefi-', dir=output) as temporary, ExitStack() as locks:
        vars_copy = Path(temporary) / 'vars.fd'
        shutil.copyfile(variables, vars_copy)
        command = [qemu, '-machine', 'q35,kernel-irqchip=split' if args.probe_iommu and args.accel == 'kvm' else 'q35', '-accel', args.accel, '-cpu', args.cpu or ('host' if args.accel == 'kvm' else 'qemu64'),
                   '-m', f'{args.memory}M', '-smp', '1', '-no-reboot', '-nic', 'none', '-monitor', 'none',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={code}',
                   '-drive', f'if=pflash,format=raw,file={vars_copy}', '-serial', f'file:{serial}']
        if args.network == 'user':
            command += ['-netdev', f'user,id=riplixnet,net=192.168.64.0/24,host=192.168.64.1,dns=192.168.64.3,'
                        f'hostfwd=tcp:127.0.0.1:{args.telnet_port}-192.168.64.250:23',
                        '-device', 'virtio-net-pci,id=riplixnic,netdev=riplixnet,disable-legacy=on,mac=52:54:00:52:50:01']
            if testing:
                command += ['-object', f'filter-dump,id=riplixtrace,netdev=riplixnet,file={output / "network.pcap"}']
        if args.usb_boot:
            command += ['-device', 'qemu-xhci', '-drive', f'if=none,id=bootusb,format=raw,snapshot=on,file={disk}',
                        '-device', 'usb-storage,drive=bootusb,removable=on']
        else:
            command += ['-drive', f'format=raw,snapshot=on,file={disk}']
        probe_before = None
        if args.probe_iommu:
            command += ['-device', 'intel-iommu,intremap=on,caching-mode=on']
        if args.nvme_probe and args.probe_expect != 'missing':
            from gpt_image import fixture, write_image, fingerprint
            probe = args.probe_disk.absolute() if args.probe_disk else Path(temporary) / 'probe.img'
            if not args.probe_disk:
                write_image(probe, fixture(args.nvme_lba))
            probe_before = fingerprint(probe)
            command += ['-blockdev', json.dumps(dict(driver='raw', **{'node-name': 'probe-disk', 'read-only': True},
                         file=dict(driver='file', filename=str(probe), **{'read-only': True}))),
                        '-device', f'nvme,drive=probe-disk,serial=RIPLIXPROBE,logical_block_size={args.nvme_lba},physical_block_size={args.nvme_lba}']
        root = None
        outside_before = None
        if args.high_framebuffer:
            command += ['-device', 'pci-testdev,membar=16M,membar-backed=on']
        if persistent:
            root = esp.parent / 'test-root.img' if testing else args.disk or output / 'rootfs.img'
            root = root.absolute()
            root.parent.mkdir(parents=True, exist_ok=True)
            lock = locks.enter_context(root.with_name(root.name + '.lock').open('a'))
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError(f'root image is already in use: {root}') from None
            if testing:
                root.unlink(missing_ok=True) # Only this test's dedicated disk, never the user's disk.
            if args.nvme_partition:
                from partition_image import create_fixture, outside_fingerprint
                if testing:
                    create_fixture(root, esp.parent / 'root.img', root_identity)
                    outside_before = outside_fingerprint(root, root_identity)
                    (output / 'test-root-identity.json').write_text(json.dumps(root_identity, indent=2) + '\n')
            else:
                create_root_image(root, esp.parent / 'root.img')
            command += ['-blockdev', json.dumps(dict(driver='raw', **{'node-name': 'riplix-root'},
                         file=dict(driver='file', filename=str(root)),
                         cache=dict(direct=False, **{'no-flush': False}))),
                        '-device', ('virtio-blk-pci,drive=riplix-root,disable-legacy=on,num-queues=1,queue-size=8,rerror=report,werror=report'
                                    if args.storage == 'virtio' else
                                    f'nvme,drive=riplix-root,serial=RIPLIXTEST,logical_block_size={args.nvme_lba},physical_block_size={args.nvme_lba}')]
        if testing:
            command += ['-display', 'none', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
        if args.debug:
            command += ['-gdb', 'tcp:127.0.0.1:1234', '-S']
        print(shlex.join(command), flush=True)
        storage_rejections = []
        with (output / 'qemu.log').open('w') as log:
            try:
                if testing and args.stage == 'riplix' and args.fault == 'none':
                    from qemu_check import run_shell_check
                    if args.network == 'user':
                        from network_check import NetworkCheck
                        network_checks = locks.enter_context(NetworkCheck(command, args.telnet_port, output, supervised=args.system_services))
                    else:
                        network_checks = None
                    result = run_shell_check(command, serial, log, args.timeout, output / 'screen.png',
                                             persistent=persistent, storage=args.storage,
                                             network_checks=network_checks, system_services=args.system_services)
                    if persistent and result.returncode == 33:
                        first = serial.read_text(errors='replace')
                        (output / 'storage-first-boot.log').write_text(first)
                        if args.network == 'user':
                            shutil.copyfile(output / 'network.pcap', output / 'network-first-boot.pcap')
                        serial.unlink()
                        result = run_shell_check(command, serial, log, args.timeout, output / 'restart-screen.png',
                                                 persistent=True, restart=True, storage=args.storage,
                                                 network_checks=network_checks, system_services=args.system_services)
                        second = serial.read_text(errors='replace')
                        (output / 'storage-second-boot.log').write_text(second)
                        serial.write_text(first + '\n--- FRESH QEMU PROCESS ---\n' + second)
                        if result.returncode == 33:
                            from storage_check import run_storage_rejections
                            storage_rejections = run_storage_rejections(command, root, output, log, args.timeout,
                                                                        storage=args.storage, root_identity=root_identity)
                else:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                            timeout=args.timeout if testing else None)
            except subprocess.TimeoutExpired:
                raise RuntimeError(f'boot timed out; inspect {serial} and {output / "qemu.log"}') from None
        if probe_before is not None and fingerprint(probe) != probe_before:
            raise RuntimeError('read-only probe changed its virtual disk image')
        if outside_before is not None and outside_fingerprint(root, root_identity) != outside_before:
            raise RuntimeError('partition root changed bytes outside its dedicated GPT partition')
        if testing:
            transcript = serial.read_text(errors='replace') if serial.exists() else ''
            expected = ['UEFI_RELOCATION_OK', 'ELF_LOADED', 'EXIT_BOOT_SERVICES_OK', 'ELF_KERNEL_ENTRY', 'LONG_MODE_OK',
                        'OWN_STACK_OK', 'GDT_IDT_TSS_OK', 'OWN_PAGE_TABLES_OK',
                        'PAGE_ALLOCATOR_OK', 'FRAMEBUFFER_OK', 'CONSOLE_SCROLL_CHECK_OK', 'RSDP_HANDOFF', 'ACPI_OK',
                        'APIC_IRQ_OK' if args.interrupt_controller == 'apic' else 'PIT_IRQ_OK', 'IRQ_CONTEXT_OK']
            if args.network == 'user':
                expected += ['VIRTIO_NET_OK', 'NETWORK_QUIESCED', 'NETWORK_REMOTE_SHELL_OK']
            if args.wifi_firmware:
                expected += ['WIFI_BRINGUP_SKIPPED no_ax211'] # Absence only, not radio acceptance.
            if args.wifi_probe:
                expected += ['WIFI_PROBE_SKIPPED no_ax211'] # QEMU does not emulate this radio.
            if args.nvme_probe:
                if args.probe_expect == 'missing':
                    expected += ['NVME_PROBE_SKIPPED missing_controller']
                else:
                    expected += ['DMA_IDENTITY_OK', 'NVME_READONLY_GUARD_OK',
                                 'NVME_PROBE_READONLY_OK' if args.probe_expect == 'ok' else 'NVME_PROBE_READONLY_FAILED',
                                 'NVME_PROBE_IO', 'writes=0 flushes=0', 'NVME_PROBE_QUIESCED']
                if args.probe_iommu and args.probe_expect != 'missing':
                    expected += ['DMAR_HANDOFF']
            if args.high_framebuffer:
                import re
                mapping = re.search(r'FRAMEBUFFER_MAP base=0x([0-9a-f]+)', transcript)
                if not mapping or int(mapping[1], 16) < 64 * 1024**3:
                    raise RuntimeError('high-framebuffer test did not exercise a physical address above 64 GiB')
                expected += ['HIGH_FRAMEBUFFER_TEST_OK']
            fault = dict(invalid_opcode=6, null_page=14, stack_overflow=8).get(args.fault)
            if fault is not None:
                expected += [f'EXCEPTION vector=0x{fault:016x}', 'BOOT_FAIL: CPU exception vector']
                if fault == 14:
                    expected += ['cr2=0x0000000000000000']
            else:
                expected += ['RIPLIX_X86_64_BOOT_OK', 'TIMER_RUNTIME_OK']
                if args.stage == 'runtime':
                    expected += ['YYYYYYYY', 'RCC_FREESTANDING_RUNTIME_OK']
                elif args.stage == 'process':
                    expected += ['RCC_FREESTANDING_PROCESS_CHECKS_OK', 'RCC_PROCESS_DRIVER_OK']
                elif args.stage == 'bootstrap':
                    expected += ['Riplix version', 'PID 1 and kernel file syscalls verified',
                                 'bootstrap check passed at native /init loader boundary', 'RIPLIX_KERNEL_BOOTSTRAP_OK']
                else:
                    expected += ['PS2_KEYBOARD_OK', 'native /init running as PID 1', 'INDEPENDENT_IMAGE_LOADED',
                                 'native process checks passed', 'pipe checks passed',
                                 'independent app checks passed', 'timer checks passed',
                                 'BOURNE_SCRIPT_OK', 'INDEPENDENT_IMAGES_OK',
                                 'RIPLIX_USERSPACE_SESSION_OK', 'filesystem synced; session stopped']
                    if persistent:
                        expected += ['NVME_BLK_OK' if args.storage == 'nvme' else 'VIRTIO_BLK_OK', 'BLOCK_DRIVER_CHECKS_OK',
                                     f'ROOT_STORAGE {args.storage} persistent', 'BLOCK_IO_OK',
                                     'PERSISTENCE_SECOND_BOOT_OK']
                        if args.nvme_partition:
                            expected += ['ROOT_PARTITION_OK', 'ROOT_PARTITION_GUARD_OK']
            print(transcript, end='')
            expected_exit = 35 if fault is not None else 33
            if result.returncode != expected_exit or any(marker not in transcript for marker in expected) or \
                    (fault is None and ('BOOT_FAIL' in transcript or 'EXCEPTION' in transcript)) or \
                    (fault is not None and 'RIPLIX_X86_64_BOOT_OK' in transcript):
                raise RuntimeError(f'boot acceptance failed (QEMU status {result.returncode}); inspect {output}')
            report.write_text(json.dumps(dict(status='passed', machine='q35', accelerator=args.accel,
                                             architecture='x86_64', firmware=str(code),
                                             memory_mib=args.memory,
                                             network=args.network, telnet_port=args.telnet_port if args.network != 'none' else None,
                                             wifi_probe=args.wifi_probe, wifi_bringup=args.wifi_firmware is not None, wifi_pnvm=args.wifi_pnvm is not None, wifi_scan=args.wifi_scan, wifi_station_setup=args.wifi_prepare_ssid is not None, wifi_auth=args.wifi_auth, wifi_assoc=args.wifi_assoc, wifi_wpa2=args.wifi_psk_file is not None, wifi_data_probe=args.wifi_data_probe,
                                             interrupt_controller=args.interrupt_controller,
                                             high_framebuffer=args.high_framebuffer,
                                             nvme_probe=args.nvme_probe, probe_expect=args.probe_expect,
                                             probe_iommu=args.probe_iommu, probe_disk_fingerprint=probe_before,
                                             kernel_irqchip="split" if args.probe_iommu and args.accel == "kvm" else "auto",
                                             framebuffer_cache=args.framebuffer_cache,
                                             boot_transport='usb-xhci' if args.usb_boot else 'sata',
                                             storage=args.storage if persistent else 'ram',
                                             nvme_lba=args.nvme_lba if args.nvme_probe or (persistent and args.storage == 'nvme') else None,
                                             persistence_boots=2 if persistent and args.fault == 'none' else 0,
                                             storage_rejections=storage_rejections,
                                             root_identity=root_identity, outside_partition=outside_before,
                                             stage=args.stage, fault=args.fault, font_scale=args.font_scale,
                                             heap_profile=HEAP_PROFILE if args.stage in ('riplix', 'bootstrap') else None,
                                             font=args.font, font_height=args.font_height,
                                             checks=expected, qemu_exit=result.returncode), indent=2) + '\n')
            print(f'PASS: freestanding x86_64 stage={args.stage} fault={args.fault}')
        elif result.returncode:
            raise RuntimeError(f'QEMU exited with status {result.returncode}; inspect {output / "qemu.log"}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'x86_64 boot: {error}') from None
