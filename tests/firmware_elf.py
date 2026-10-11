"""Read local decrypted Vita ELF modules without distributing firmware data."""
from pathlib import Path
import hashlib
import struct


class FirmwareElf:
    def __init__(self, path, parse_imports=True):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        if self.data[:7] != b"\x7fELF\x01\x01\x01":
            raise ValueError(f"Not a decrypted little-endian ARM ELF: {self.path}")
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", self.data)
        if header[2] != 40:
            raise ValueError(f"Not an ARM module: {self.path}")
        self.segments = [struct.unpack_from("<8I", self.data,
                         header[5] + i * header[9]) for i in range(header[10])]
        self.info_segment = header[4] >> 30
        self.info_offset = header[4] & 0x3fffffff
        info = self.segment(self.info_segment)[self.info_offset:]
        self.name = info[4:31].split(b"\0", 1)[0].decode()
        self.nid = struct.unpack_from("<I", info, 0x34)[0]
        self.text = self.segment(0)
        self.text_base = self.segments[0][2]
        self.data_base = self.segments[1][2]
        self.text_size = self.segments[0][5]
        self.data_size = self.segments[1][5]
        self.exports = {}
        start, end = struct.unpack_from("<II", info, 0x24)
        while start < end:
            size, _, _, functions = struct.unpack_from("<4H", self.text, start)
            size &= 0xff  # Versioned export records encode the version in byte 1.
            if size != 0x20:
                raise ValueError(f"Unexpected export record size {size:#x}")
            library, _, nids, entries = struct.unpack_from("<4I", self.text, start + 16)
            variables = struct.unpack_from("<H", self.text, start + 8)[0]
            for i in range(functions + variables):
                nid = struct.unpack("<I", self.read(nids + 4*i, 4))[0]
                address = struct.unpack("<I", self.read(entries + 4*i, 4))[0]
                self.exports[library, nid] = address
            start += size
        self.imports = {}
        start, end = struct.unpack_from("<II", info, 0x2c)
        if not parse_imports:
            end = start
        while start < end:
            size, _, _, functions = struct.unpack_from("<4H", self.text, start)
            if size != 0x34:
                raise ValueError(f"Unexpected import record size {size:#x}")
            library = struct.unpack_from("<I", self.text, start + 16)[0]
            nids, entries = struct.unpack_from("<II", self.text, start + 28)
            for i in range(functions):
                nid = struct.unpack("<I", self.read(nids + 4*i, 4))[0]
                address = struct.unpack("<I", self.read(entries + 4*i, 4))[0]
                self.imports[library, nid] = address
            start += size

    def segment(self, index, with_bss=False):
        segment = self.segments[index]
        result = self.data[segment[1]:segment[1] + segment[4]]
        return result.ljust(segment[5], b"\0") if with_bss else result

    def read(self, address, size):
        for segment in self.segments:
            if segment[0] == 1 and segment[2] <= address and address + size <= segment[2] + segment[4]:
                offset = segment[1] + address - segment[2]
                return self.data[offset:offset + size]
        raise ValueError(f"Unmapped address {address:#x} in {self.path}")

    def identity(self):
        return {"name": self.name, "nid": f"0x{self.nid:08X}",
                "text_size": self.text_size, "data_size": self.data_size,
                "sha256": hashlib.sha256(self.data).hexdigest(),
                "text_sha256": hashlib.sha256(self.text).hexdigest()}
