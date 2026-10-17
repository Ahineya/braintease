"""Independent read-only C16 decoder, mapping disk pages instead of copying images."""
import mmap
import struct


class RootReader:
    def __init__(self, path):
        with open(path, 'rb') as file:
            self.disk = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
        disk = self.disk
        u16 = lambda at: struct.unpack_from('<H', disk, at)[0]
        bps, spc = u16(11), disk[13]
        fat = u16(14) * bps
        root = fat + disk[16] * u16(22) * bps
        self.start = root + ((u16(17) * 32 + bps - 1) // bps) * bps
        self.cluster_bytes = bps * spc
        for index in range(u16(17)):
            entry = root + index * 32
            if disk[entry:entry + 11] == b'ROOTFS  IMG':
                cluster, self.size = u16(entry + 26), struct.unpack_from('<I', disk, entry + 28)[0]
                break
        else:
            raise AssertionError('missing ROOTFS.IMG')
        self.clusters, seen = [], set()
        while len(self.clusters) * self.cluster_bytes < self.size:
            assert 2 <= cluster < 0xfff0 and cluster not in seen, 'invalid FAT chain'
            self.clusters.append(cluster)
            seen.add(cluster)
            cluster = u16(fat + cluster * 2)
        assert self.word(1024) == 0x7254, 'expected C16 V3'

    def word(self, cell):
        offset = cell * 2
        assert 0 <= offset < self.size
        cluster, within = divmod(offset, self.cluster_bytes)
        return struct.unpack_from('<H', self.disk,
                                  self.start + (self.clusters[cluster] - 2) * self.cluster_bytes + within)[0]

    def wide(self, cell):
        return self.word(cell) + 65536 * self.word(cell + 1)

    @staticmethod
    def base(ino):
        return (2 + ino // 16) * 1024 + (ino % 16) * 64

    def block(self, ino, index):
        base = self.base(ino)
        if index < 17:
            return self.wide(base + 12 + index * 2)
        index -= 17
        span = 512
        for depth in range(1, 4):
            if index < span:
                pointer = self.wide(base + 12 + (17 + depth - 1) * 2)
                divisor = span // 512
                while divisor:
                    if not pointer:
                        return 0
                    slot, index = divmod(index, divisor)
                    pointer = self.wide(pointer * 1024 + slot * 2)
                    divisor //= 512
                return pointer
            index -= span
            span *= 512
        raise AssertionError('invalid logical block')

    def cells(self, ino):
        size = self.wide(self.base(ino) + 4)
        result = []
        for index in range((size + 1023) // 1024):
            block = self.block(ino, index)
            count = min(1024, size - len(result))
            result.extend(self.word(block * 1024 + i) if block else 0 for i in range(count))
        return result

    def lookup(self, path):
        ino = 1
        for name in filter(None, path.split('/')):
            data = self.cells(ino)
            for at in range(0, len(data), 32):
                if data[at] and ''.join(map(chr, data[at + 1:at + 31])).split('\0')[0] == name:
                    ino = data[at]
                    break
            else:
                raise AssertionError('missing ' + path)
        return ino

    def read(self, path):
        return self.cells(self.lookup(path))
