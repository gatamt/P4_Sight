# Wire protocol

All integer fields are little-endian unless stated otherwise.

## Video and control port

UDP port: `3334`

### Four-byte control messages

| Bytes | Meaning |
| --- | --- |
| `VID0` | Register or re-register the receiving client |
| `VACK` | Registration acknowledgement |
| `BEAT` | Heartbeat |
| `PAWS` | Pause streaming |
| `GONE` | End the session |

The source address of `VID0` becomes the current video destination.

## H.264 packet header

Each datagram contains a 28-byte header followed by up to 1400 bytes of H.264
payload.

| Offset | Size | Field | Description |
| ---: | ---: | --- | --- |
| 0 | 4 | `frame_id` | Monotonic encoded-frame identifier |
| 4 | 2 | `width` | Frame width |
| 6 | 2 | `height` | Frame height |
| 8 | 4 | `timestamp` | Firmware monotonic time in milliseconds |
| 12 | 4 | `total_len` | Complete encoded-frame length |
| 16 | 2 | `chunk_idx` | Zero-based chunk index |
| 18 | 2 | `chunk_count` | Number of chunks in this frame |
| 20 | 4 | `frame_type` | `1` for a keyframe, otherwise `0` |
| 24 | 4 | `reserved` | Reserved, currently zero |

The receiver places each payload at `chunk_idx * 1400`. It must verify the total
length and wait for all unique chunk indexes before decoding.

## Detection metadata

UDP port: `3335`

A detection packet starts with:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII `BBOX` |
| 4 | 4 | frame ID |
| 8 | 4 | timestamp in milliseconds |
| 12 | 1 | detection count |

Each detection record is 26 bytes:

| Relative offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | class ID |
| 1 | 1 | confidence in percent |
| 2 | 2 | x1 |
| 4 | 2 | y1 |
| 6 | 2 | x2 |
| 8 | 2 | y2 |
| 10 | 1 | label length, maximum 15 |
| 11 | 15 | UTF-8 label storage |

Coordinates are expressed in the 1280 x 720 source-frame coordinate system.
