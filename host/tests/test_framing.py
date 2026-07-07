import pytest

from giga_host.framing import (
    FrameParser,
    FramingError,
    cobs_decode,
    cobs_encode,
    crc16_ccitt,
    encode_frame,
)


def test_crc16_check_value():
    # The published CRC-16/CCITT-FALSE check value.
    assert crc16_ccitt(b"123456789") == 0x29B1


@pytest.mark.parametrize(
    "data",
    [
        b"",
        b"\x00",
        b"\x00\x00",
        b"\x11\x22\x00\x33",
        b"\x11" * 254,
        b"\x11" * 255,
        b"\x00" * 300,
        bytes(range(256)) * 5,
    ],
)
def test_cobs_roundtrip(data):
    encoded = cobs_encode(data)
    assert 0 not in encoded
    assert cobs_decode(encoded) == data


def test_cobs_decode_rejects_truncated_block():
    encoded = cobs_encode(b"\x11\x22\x33")
    with pytest.raises(FramingError):
        cobs_decode(encoded[:-1])


def test_parser_roundtrip_and_resync():
    parser = FrameParser()
    frame = encode_frame(b"\x01hello")
    garbage = b"\x55\xaa\x13"  # partial junk, then delimiter resyncs
    payloads = parser.feed(garbage + b"\x00" + frame + frame)
    assert payloads == [b"\x01hello", b"\x01hello"]
    assert parser.crc_err_count == 1


def test_parser_drops_corrupt_frame():
    parser = FrameParser()
    frame = bytearray(encode_frame(b"\x01hello"))
    frame[2] ^= 0x01  # 'h' -> 'i': corrupts payload without creating a 0x00
    assert parser.feed(bytes(frame)) == []
    assert parser.crc_err_count == 1


def test_parser_byte_at_a_time():
    parser = FrameParser()
    frame = encode_frame(bytes(range(1, 100)))
    payloads = []
    for i in range(len(frame)):
        payloads += parser.feed(frame[i : i + 1])
    assert payloads == [bytes(range(1, 100))]


def test_parser_oversized_frame_dropped():
    parser = FrameParser()
    assert parser.feed(b"\x01" * 5000 + b"\x00") == []
    assert parser.crc_err_count == 1
    # and the parser still works afterwards
    assert parser.feed(encode_frame(b"\x01ok")) == [b"\x01ok"]
