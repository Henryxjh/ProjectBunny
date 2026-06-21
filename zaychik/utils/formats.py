"""DXGI format descriptors and typed decoders for vertex/index buffer contents.

Reference:
- https://learn.microsoft.com/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format
- SSMT's hardcoded format tables (ssmt4/src-tauri/src/constants/gametype_format.rs).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Callable, Dict, Optional, Tuple


# A decoded element is a tuple of floats (for unorm/snorm/float types) or ints
# (for uint/sint types).
Components = Tuple


@dataclass(frozen=True)
class FormatInfo:
    name: str
    byte_width: int
    component_count: int
    is_integer: bool
    decode: Callable[[bytes, int], Components]


class DxgiFormat:
    """Registry of DXGI formats used by vertex/index buffers.

    Populated once at class-load time via ``_build()``; accessed via the
    ``DxgiFormat.get()`` classmethod.
    """

    _TABLE: Dict[str, FormatInfo] = {}

    @classmethod
    def _populate(cls) -> None:
        if cls._TABLE:
            return

        def add(name: str, byte_width: int, component_count: int, is_integer: bool,
                decode: Callable[[bytes, int], Components]) -> None:
            cls._TABLE[name] = FormatInfo(name, byte_width, component_count, is_integer, decode)

        def mk_unpack(st: str, count: int, is_integer: bool) -> Tuple[int, int, bool, Callable]:
            fmt = "<" + st
            bw = struct.calcsize(fmt)

            def _decode(data: bytes, offset: int) -> Components:
                return struct.unpack_from(fmt, data, offset)

            return bw, count, is_integer, _decode

        # UNORM/SNORM helpers
        def u8(b: int) -> float: return b / 255.0
        def s8(b: int) -> float: return max(-1.0, b / 127.0)
        def u16(v: int) -> float: return v / 65535.0
        def s16(v: int) -> float: return max(-1.0, v / 32767.0)

        def decode_rgba8_unorm(d, o):
            return (u8(d[o]), u8(d[o+1]), u8(d[o+2]), u8(d[o+3]))
        def decode_rgba8_snorm(d, o):
            b0, b1, b2, b3 = struct.unpack_from("<4b", d, o)
            return (s8(b0), s8(b1), s8(b2), s8(b3))
        def decode_bgra8_unorm(d, o):
            return (u8(d[o+2]), u8(d[o+1]), u8(d[o]), u8(d[o+3]))
        def decode_empty(d, o):
            del d, o
            return ()

        add("DXGI_FORMAT_UNKNOWN", 0, 0, False, decode_empty)

        # Indices / scalar ints
        add("DXGI_FORMAT_R32_UINT", *mk_unpack("I", 1, True))
        add("DXGI_FORMAT_R16_UINT", *mk_unpack("H", 1, True))
        add("DXGI_FORMAT_R8_UINT",  1, 1, True, lambda d, o: (d[o],))
        add("DXGI_FORMAT_R8_SINT",  1, 1, True, lambda d, o: struct.unpack_from("<b", d, o))

        # Float32
        add("DXGI_FORMAT_R32_FLOAT",          *mk_unpack("f",  1, False))
        add("DXGI_FORMAT_R32G32_FLOAT",       *mk_unpack("2f", 2, False))
        add("DXGI_FORMAT_R32G32B32_FLOAT",    *mk_unpack("3f", 3, False))
        add("DXGI_FORMAT_R32G32B32A32_FLOAT", *mk_unpack("4f", 4, False))

        # SInt / UInt 32
        add("DXGI_FORMAT_R32_SINT",             *mk_unpack("i",  1, True))
        add("DXGI_FORMAT_R32G32_SINT",          *mk_unpack("2i", 2, True))
        add("DXGI_FORMAT_R32G32B32_SINT",       *mk_unpack("3i", 3, True))
        add("DXGI_FORMAT_R32G32B32A32_SINT",    *mk_unpack("4i", 4, True))
        add("DXGI_FORMAT_R32G32_UINT",          *mk_unpack("2I", 2, True))
        add("DXGI_FORMAT_R32G32B32_UINT",       *mk_unpack("3I", 3, True))
        add("DXGI_FORMAT_R32G32B32A32_UINT",    *mk_unpack("4I", 4, True))

        # Float16 (half)
        add("DXGI_FORMAT_R16_FLOAT",             2, 1, False, lambda d, o: struct.unpack_from("<e", d, o))
        add("DXGI_FORMAT_R16G16_FLOAT",          4, 2, False, lambda d, o: struct.unpack_from("<ee", d, o))
        add("DXGI_FORMAT_R16G16B16A16_FLOAT",    8, 4, False, lambda d, o: struct.unpack_from("<eeee", d, o))

        # 16-bit UNORM / SNORM / UINT / SINT
        add("DXGI_FORMAT_R16_UNORM",  2, 1, False,
            lambda d, o: (u16(struct.unpack_from("<H", d, o)[0]),))
        add("DXGI_FORMAT_R16_SNORM",  2, 1, False,
            lambda d, o: (s16(struct.unpack_from("<h", d, o)[0]),))
        add("DXGI_FORMAT_R16G16_UNORM", 4, 2, False,
            lambda d, o: (u16(struct.unpack_from("<H", d, o)[0]),
                          u16(struct.unpack_from("<H", d, o+2)[0])))
        add("DXGI_FORMAT_R16G16_SNORM", 4, 2, False,
            lambda d, o: (s16(struct.unpack_from("<h", d, o)[0]),
                          s16(struct.unpack_from("<h", d, o+2)[0])))
        add("DXGI_FORMAT_R16G16B16A16_UNORM", 8, 4, False,
            lambda d, o: tuple(u16(struct.unpack_from("<H", d, o+2*i)[0]) for i in range(4)))
        add("DXGI_FORMAT_R16G16B16A16_SNORM", 8, 4, False,
            lambda d, o: tuple(s16(struct.unpack_from("<h", d, o+2*i)[0]) for i in range(4)))
        add("DXGI_FORMAT_R16_SINT",             *mk_unpack("h",  1, True))
        add("DXGI_FORMAT_R16G16_SINT",          *mk_unpack("2h", 2, True))
        add("DXGI_FORMAT_R16G16B16A16_SINT",    *mk_unpack("4h", 4, True))
        add("DXGI_FORMAT_R16G16_UINT",          *mk_unpack("2H", 2, True))
        add("DXGI_FORMAT_R16G16B16A16_UINT",    *mk_unpack("4H", 4, True))

        # 8-bit per channel
        add("DXGI_FORMAT_R8_UNORM", 1, 1, False, lambda d, o: (u8(d[o]),))
        add("DXGI_FORMAT_R8_SNORM", 1, 1, False,
            lambda d, o: (s8(struct.unpack_from("<b", d, o)[0]),))
        add("DXGI_FORMAT_R8G8_UNORM", 2, 2, False,
            lambda d, o: (u8(d[o]), u8(d[o+1])))
        add("DXGI_FORMAT_R8G8_SNORM", 2, 2, False,
            lambda d, o: (s8(struct.unpack_from("<b", d, o)[0]),
                          s8(struct.unpack_from("<b", d, o+2)[0])))
        add("DXGI_FORMAT_R8G8B8A8_UNORM", 4, 4, False, decode_rgba8_unorm)
        add("DXGI_FORMAT_R8G8B8A8_SNORM", 4, 4, False, decode_rgba8_snorm)
        add("DXGI_FORMAT_R8G8B8A8_UINT",  4, 4, True,
            lambda d, o: (d[o], d[o+1], d[o+2], d[o+3]))
        add("DXGI_FORMAT_R8G8B8A8_SINT",  4, 4, True,
            lambda d, o: struct.unpack_from("<4b", d, o))
        add("DXGI_FORMAT_B8G8R8A8_UNORM", 4, 4, False, decode_bgra8_unorm)

    @classmethod
    def get(cls, name: Optional[str]) -> Optional[FormatInfo]:
        """Look up a DXGI format by name; returns None for UNKNOWN / missing."""
        cls._populate()
        if not name:
            return None
        return cls._TABLE.get(name.upper())

    @classmethod
    def all_formats(cls) -> Dict[str, FormatInfo]:
        cls._populate()
        return dict(cls._TABLE)


# Populate eagerly on import so FormatInfo is available for the layouts module.
DxgiFormat._populate()
