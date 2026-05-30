## Status

Runnable today:

| API | Return | Notes |
| --- | --- | --- |
| `std.codec.crc32(bytes)` | `u32` | Computes CRC-32 for a string-backed byte input. |
| `std.codec.crc32Bytes(bytes)` | `u32` | Computes CRC-32 for a span or mutable span without allocation. |
| `std.codec.encodedVarintLen(value)` | `usize` | Returns the byte length of an unsigned varint encoding. |
| `std.codec.readU8(bytes)` | `u8` | Reads one byte. |
| `std.codec.readU16(bytes)` | `u16` | Reads two bytes as little-endian. |
| `std.codec.readU32(bytes)` | `u32` | Reads four bytes as little-endian. |
| `std.codec.writeU16(value)` | `u32` | Packs a `u16` value into the current write representation. |
| `std.codec.writeU32(value)` | `u32` | Packs a `u32` value into the current write representation. |
| `std.codec.base64EncodedLen(len)` | `usize` | Returns the encoded length for a base64 payload. |
| `std.codec.base64Encode(buffer, bytes)` | `Maybe<String>` | Writes base64 text into caller storage. |
| `std.codec.hexEncode(buffer, bytes)` | `Maybe<String>` | Writes lowercase hexadecimal text into caller storage. |
| `std.codec.utf8Valid(bytes)` | `Bool` | Validates a byte span as UTF-8. |
| `std.codec.urlEncode(buffer, text)` | `Maybe<String>` | Percent-encodes a string into caller storage. |

Current limits:

- Buffer-backed write APIs.
- Error-producing reads for short inputs.
- Streaming encoders and decoders.

## Example

```zero
use std.codec

use std.mem

pub fn main(world: World) -> Void raises {
    let len: usize = std.codec.encodedVarintLen(300)
    let checksum: u32 = std.codec.crc32("zero")
    let bytes: Span<u8> = std.mem.span("zero")
    let byte_checksum: u32 = std.codec.crc32Bytes(bytes)
    if len == 2 && checksum == byte_checksum {
        check world.out.write("codec primitives ok\n")
    }
}
```

## Design Notes

The current helpers are intentionally narrow. They prove integer widths and
deterministic byte math before allocator-backed buffers are added.
