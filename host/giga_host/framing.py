"""COBS framing + CRC-16/CCITT-FALSE, per protocol/PROTOCOL.md.

Hand-written on purpose (house rule VIII): each half replaces <15 lines of
a dependency we'd have to pin forever.
"""

DELIMITER = 0x00
MAX_FRAME = 2048


class FramingError(ValueError):
    pass


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no xorout."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data):
            raise FramingError("bad COBS block")
        out += data[i + 1 : i + code]
        i += code
        if code < 255 and i < len(data):
            out.append(0)
    return bytes(out)


def encode_frame(payload: bytes) -> bytes:
    """payload (without CRC) -> full wire frame including 0x00 terminator."""
    crc = crc16_ccitt(payload)
    return cobs_encode(payload + crc.to_bytes(2, "little")) + bytes([DELIMITER])


class FrameParser:
    """Feed raw bytes, get back CRC-verified payloads (CRC stripped).

    Corrupt or oversized frames are dropped and counted in crc_err_count;
    the stream resynchronizes at the next 0x00 by construction.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._overflow = False
        self.crc_err_count = 0

    def feed(self, data: bytes) -> list[bytes]:
        payloads = []
        for byte in data:
            if byte != DELIMITER:
                if len(self._buf) >= MAX_FRAME:
                    self._overflow = True
                else:
                    self._buf.append(byte)
                continue
            encoded, self._buf = bytes(self._buf), bytearray()
            overflowed, self._overflow = self._overflow, False
            if not encoded:  # idle delimiter between frames
                continue
            if overflowed:
                self.crc_err_count += 1
                continue
            try:
                decoded = cobs_decode(encoded)
            except FramingError:
                self.crc_err_count += 1
                continue
            if len(decoded) < 3 or crc16_ccitt(decoded[:-2]) != int.from_bytes(
                decoded[-2:], "little"
            ):
                self.crc_err_count += 1
                continue
            payloads.append(decoded[:-2])
        return payloads
