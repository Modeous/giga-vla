"""Serial link to the Giga: framing + message dispatch over a byte transport.

The transport is anything with non-blocking read(n)->bytes and write(bytes)
(a pyserial Serial with timeout=0, or a pty shim in tests). Port discovery
is by USB serial number, never by /dev/ttyACM path (see ARCHITECTURE.md).
"""

from . import framing, messages


class Link:
    def __init__(self, transport) -> None:
        self._transport = transport
        self._parser = framing.FrameParser()

    @classmethod
    def open(cls, usb_serial_number: str, baud: int = 115200) -> "Link":
        import serial
        from serial.tools import list_ports

        for port in list_ports.comports():
            if port.serial_number == usb_serial_number:
                return cls(serial.Serial(port.device, baud, timeout=0))
        raise FileNotFoundError(
            f"no serial port with USB serial number {usb_serial_number!r}"
        )

    @property
    def crc_err_count(self) -> int:
        return self._parser.crc_err_count

    def send_chunk(self, chunk: messages.ActionChunk) -> None:
        self._transport.write(framing.encode_frame(chunk.pack()))

    def send_enable(self, enable: bool) -> None:
        self._transport.write(framing.encode_frame(messages.Enable(enable).pack()))

    def poll(self, max_bytes: int = 4096) -> list[messages.State]:
        """Drain available bytes; return decoded STATE messages in order."""
        data = self._transport.read(max_bytes)
        states = []
        if data:
            for payload in self._parser.feed(data):
                msg = messages.unpack_any(payload)
                if isinstance(msg, messages.State):
                    states.append(msg)
        return states
