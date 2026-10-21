"""Exercise the real PS/2 -> TTY -> shell path through QMP key events."""
import json
import os
import re
import select
import subprocess
import time


def run_shell_check(command, serial, log, timeout, screenshot, *, persistent=False, restart=False, debug_exit=True, storage='virtio', extra_checks=(), existing_root=False, checks_only=False, network_checks=None, system_services=False, exit_command='shutdown', console_input_check=False, terminal_checks=None):
    command = [*command, '-qmp', 'stdio']
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=log, bufsize=0)
    started = time.monotonic()
    deadline = started + timeout
    pending = bytearray()

    def transcript():
        return serial.read_text(errors='replace') if serial.exists() else ''

    def packet():
        while b'\n' not in pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([process.stdout], [], [], max(0, remaining))[0]:
                raise RuntimeError('QMP timed out during userspace acceptance')
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                raise RuntimeError('QEMU stopped during userspace acceptance; inspect serial.log')
            pending.extend(chunk)
        line, _, rest = pending.partition(b'\n')
        pending[:] = rest
        return json.loads(line)

    def qmp(name, arguments=None):
        request = {'execute': name}
        if arguments is not None:
            request['arguments'] = arguments
        process.stdin.write(json.dumps(request).encode() + b'\n')
        process.stdin.flush()
        while True:
            result = packet()
            if 'error' in result:
                raise RuntimeError(f'QMP {name}: {result["error"]}')
            if 'return' in result:
                return result['return']

    def wait_for(predicate):
        while True:
            text = transcript()
            if 'BOOT_FAIL' in text or 'EXCEPTION vector=' in text or 'CHECK FAILED:' in text:
                raise RuntimeError('guest failure during userspace acceptance; inspect serial.log')
            if 'BdsDxe: failed to load' in text and 'Not Found' in text and 'ELF_KERNEL_ENTRY' not in text and time.monotonic()-started>20:
                raise RuntimeError('firmware failed to load USB image before kernel entry')
            if predicate(text):
                return text
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('userspace acceptance timed out or stopped; inspect serial.log')
            time.sleep(0.01)

    def type_text(text):
        keys = {' ': 'spc', '\n': 'ret', '\b': 'backspace', '/': 'slash', '-': 'minus',
                '.': 'dot', '=': 'equal', ',': 'comma', ';': 'semicolon',
                "'": 'apostrophe', '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash'}
        shifts = {'_': '-', '+': '=', ':': ';', '"': "'", '!': '1', '?': '/',
                  '{': '[', '}': ']', '^': '6', '%': '5', '#': '3', '~': '`',
                  '$': '4', '|': '\\', '>': '.', '<': ',', '&': '7', '(': '9', ')': '0', '*': '8'}
        keys['`'] = 'grave_accent'
        for char in text:
            if time.monotonic() >= deadline:
                raise RuntimeError('keyboard test exceeded timeout')
            shifted = char.isupper() or char in shifts
            base = shifts.get(char, char.lower())
            key = base if base.isascii() and base.isalnum() else keys[base]
            events = []
            def event(code, down):
                events.append({'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}})
            if shifted:
                event('shift', True)
            event(key, True); event(key, False)
            if shifted:
                event('shift', False)
            qmp('input-send-event', {'events': events})
            # The bootstrap keyboard is polled; leave enough guest time under
            # TCG plus background services to drain make/break scan codes.
            time.sleep(0.2 if system_services else 0.025)

    def shell(line, expected):
        start = len(transcript())
        type_text(line + '\n')
        text = wait_for(lambda text: 'fuse-x# ' in text[start:])
        output = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', text[start:])
        if expected and not re.search(expected, output):
            raise RuntimeError(f'shell command {line!r} failed: {output!r}')
        if 'errno ' in output or 'CHECK FAILED:' in output:
            raise RuntimeError(f'shell command {line!r} reported an error: {output!r}')
        print(f'PASS: PS/2 shell command {line!r}', flush=True)
        return output

    def finish_session():
        type_text(exit_command+'\n')
        wait_for(lambda text: ('PLATFORM_REBOOT' if exit_command=='reboot' else 'RIPLIX_X86_64_BOOT_OK') in text)
        if not debug_exit and exit_command!='reboot':
            qmp('quit') # Normal images halt instead of writing the test exit port.
        return subprocess.CompletedProcess(command, process.wait(timeout=max(0.1, deadline - time.monotonic())))

    try:
        if 'QMP' not in packet():
            raise RuntimeError('QMP greeting missing')
        qmp('qmp_capabilities')
        wait_for(lambda text: 'fuse-x# ' in text)
        if 'PS2_KEYBOARD_OK' not in transcript():
            raise RuntimeError('PS/2 keyboard did not initialize')
        if persistent and f'ROOT_STORAGE {storage} persistent' not in transcript():
            raise RuntimeError(f'persistent root was not mounted through {storage}')
        if system_services:
            shell('svcd --check --config /etc/services --profile /etc/boot/services.ini', r'network enabled[\s\S]*telnetd enabled')
            shell('svc --runtime /var/run/svcd status', r'network (done|failed|backoff|starting|waiting)[\s\S]*telnetd (running|blocked|waiting|starting)')
        if network_checks:
            network_checks.run(shell, transcript, deadline, restart, qmp)
        if console_input_check:
            dimensions = re.search(r'CONSOLE_GEOMETRY rows=(0x[0-9a-fA-F]+) columns=(0x[0-9a-fA-F]+)', transcript())
            if not dimensions:
                raise RuntimeError('native console dimensions were not reported')
            rows, columns = (int(value, 16) for value in dimensions.groups())
            shell('stty -a', rf'\b{rows} rows {columns} columns\b')
            print(f'PASS: physical TIOCGWINSZ matches native console {columns}x{rows}', flush=True)
            # Exercise raw terminal octets through QMP/i8042, the native adapter,
            # TTY, shell read and filesystem. No new guest executable is needed.
            start = len(transcript())
            type_text('echo KEY_CAPTURE_READY; read keys; echo "$keys" > /tmp/keyboard-octets; wc -c /tmp/keyboard-octets\n')
            wait_for(lambda text: re.search(r'\r?\nKEY_CAPTURE_READY\r?\n', text[start:]))
            for code in ('esc', 'up', 'f1', 'f5', 'f12', 'ret'):
                qmp('input-send-event', {'events': [
                    {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}
                    for down in (True, False)]})
                time.sleep(0.2)
            captured = wait_for(lambda text: 'fuse-x# ' in text[start:])[start:]
            if not re.search(r'\r?\n\s*18\s+/tmp/keyboard-octets', captured):
                raise RuntimeError(f'keyboard capture length mismatch: {captured!r}')
            start = len(transcript())
            shell('echo KEY_BYTES_BEGIN; cat /tmp/keyboard-octets; echo KEY_BYTES_END', '')
            raw = transcript()[start:]
            expected = b'\x1b\x1b[A\x1bOP\x1b[15~\x1b[24~'
            if not re.search(r'\r?\nKEY_BYTES_BEGIN\r?\n' + re.escape(expected.decode()) +
                             r'\r?\nKEY_BYTES_END\r?\n', raw):
                raise RuntimeError(f'keyboard terminal octets mismatch: {raw!r}')
            print('PASS: PS/2 Escape/Up/F1/F5/F12 exact terminal octets', flush=True)
        for line, expected in extra_checks:
            shell(line, expected)
        if terminal_checks:
            terminal_checks(shell, type_text, qmp, wait_for, transcript)
        if checks_only:
            qmp('screendump', {'filename': str(screenshot), 'format': 'png'})
            return finish_session()
        if restart:
            shell('cat /root/persist', r'\r?\nRIPLIX_DISK_SURVIVED\r?\n')
            shell('cat /tmp/note', r'\r?\nhello-from-ram\r?\n')
            shell('/root/saved-echo after-reboot', r'\r?\nafter-reboot\r?\n')
            shell('diskcheck --verify', r'disk checks passed')
            shell('echo PERSISTENCE_SECOND_BOOT_OK', r'\r?\nPERSISTENCE_SECOND_BOOT_OK\r?\n')
            qmp('screendump', {'filename': str(screenshot), 'format': 'png'})
            return finish_session()
        shell('cd /bin', '')
        shell('ls', r'appcheck')
        shell('cd /root', '')
        shell('echo Keyboard_4x\b2', r'\r?\nKeyboard_42\r?\n')
        shell('echo $$', r'\r?\n[0-9]+\r?\n' if system_services else r'\r?\n2\r?\n')
        listing = shell('ls /tmp', '')
        if not existing_root and re.search(r'\r?\n(?:note|context-check|not-native)\r?\n', listing):
            raise RuntimeError('RAM root retained files from an earlier session')
        shell('ls /bin', r'appcheck')
        shell('hello freestanding', r'Hello from native Riplix! PID \d+ freestanding')
        shell('echo hello-from-ram > /tmp/note', '')
        shell('cat /tmp/note', r'\r?\nhello-from-ram\r?\n')
        shell('check', r'native process checks passed \(fork/COW, wait, shared files, timers, exec and slot reuse\)')
        shell('pipecheck', r'pipe checks passed \(20,000 cells, blocking, wrap, EOF and wait\)')
        shell('appcheck', r'independent app checks passed')
        shell('abicheck', r'\r?\nYYYY\r?\n')
        shell('timercheck', r'timer checks passed \(busy loop, sleeping child, signal and wait\)')
        shell('sh /demo.sh', r'BOURNE_SCRIPT_OK')
        shell('/demo.sh', r'BOURNE_SCRIPT_OK')
        shell('echo shell-pipe | cat', r'\r?\nshell-pipe\r?\n')
        shell('for x in one two; do echo item-$x; done', r'item-one\r?\nitem-two')
        shell('false; echo status-$?', r'\r?\nstatus-1\r?\n')
        shell('echo `echo substitution`', r'\r?\nsubstitution\r?\n')
        shell('cp /bin/echo /tmp/renamed; /tmp/renamed copied-app', r'\r?\ncopied-app\r?\n')
        shell('if /missing; then echo BAD; else echo EXEC_RECOVERED; fi', r'EXEC_RECOVERED')
        before = time.monotonic()
        shell('sleep 1', '')
        if time.monotonic() - before < 0.9:
            raise RuntimeError('sleep returned before its timer could expire')
        shell('cat /README', r'Use shutdown to flush files before stopping the machine\.')
        if persistent:
            shell('echo RIPLIX_DISK_SURVIVED > /root/persist', '')
            shell('cp /bin/echo /root/saved-echo', '')
            # More than one DMA buffer and filesystem block, with exact byte
            # contents checked in the guest on both boots by diskcheck below.
            shell('diskcheck', r'disk checks passed')
        qmp('screendump', {'filename': str(screenshot), 'format': 'png'})
        return finish_session()
    except Exception:
        # Retain native failure state before disposing of this private VM.
        try:
            match = re.search(r'ELF_LOADED entry=0x([0-9a-f]+)', transcript())
            if match:
                qmp('pmemsave', {'val':int(match[1],16), 'size':0x300000,
                                 'filename':str(screenshot.parent / 'kernel-memory.bin')})
            state = qmp('human-monitor-command', {'command-line': 'info registers'})
            log.write(str(state)); log.flush()
            qmp('screendump', {'filename': str(screenshot), 'format': 'png'})
        except Exception:
            pass
        raise
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdin.close(); process.stdout.close()
