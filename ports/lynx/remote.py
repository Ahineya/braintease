"""Telnet terminal driven through the independent Ethernet test peer."""
from guest import Screen


class Remote:
    def __init__(self, guest, local_port):
        self.guest = guest
        self.stream = guest.peer.connect(23, local_port)
        self.screen = Screen()
        self.at = self.raw_at = 0
        self.pending = bytearray()
        guest.wait(lambda: self.stream.established, 'Telnet connection established')
        self.send(b'\xff\xfd\x01\xff\xfd\x03\xff\xfb\x1f\xff\xfa\x1f\0P\0\x18\xff\xf0')
        self.raw(b'riplix# ')

    def feed(self):
        self.pending.extend(self.stream.received[self.at:])
        self.at = len(self.stream.received)
        data, i = bytearray(), 0
        while i < len(self.pending):
            ch = self.pending[i]
            if ch != 255:
                data.append(ch); i += 1; continue
            if i + 1 == len(self.pending): break
            command = self.pending[i + 1]
            if command == 255:
                data.append(255); i += 2
            elif command in (251, 252, 253, 254):
                if i + 2 == len(self.pending): break
                i += 3
            elif command == 250:
                end = self.pending.find(b'\xff\xf0', i + 2)
                if end < 0: break
                i = end + 2
            else: i += 2
        del self.pending[:i]
        self.screen.feed(bytes(data))

    def send(self, data):
        self.stream.queue(data)

    def raw(self, marker):
        self.guest.wait(lambda: marker in bytes(self.stream.received[self.raw_at:]).replace(b'\r', b''), repr(marker))
        data = bytes(self.stream.received[self.raw_at:]).replace(b'\r', b'')
        # Callers wait for the complete prompt at the end of a command, so no
        # following command's output can have arrived yet.
        self.raw_at = len(self.stream.received)
        self.feed()
        return data

    def shell(self, command):
        self.send(command.encode() + b'\r\n')
        return self.raw(b'riplix# ')

    def visible(self, text):
        def seen():
            self.feed()
            return text in self.screen.text()
        self.guest.wait(seen, text)
