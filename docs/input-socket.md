# TMTV_INPUT_SOCKET — Per-User Input API

tmtv exposes a Unix domain socket that delivers per-user input events to programs running inside a session. Each viewer (SSH or web) gets a unique identity, so your program knows *who* typed *what*.

Use cases: multiplayer terminal games, in-session chat, collaborative editors, voting systems, anything where you need to distinguish between viewers.

## Discovering the socket

When a tmtv session starts, the client creates a Unix socket at `/tmp/tmtv-input-<pid>.sock` and exports its path:

```sh
echo $TMTV_INPUT_SOCKET
# /tmp/tmtv-input-12345.sock
```

The socket exists for the lifetime of the tmtv session. It is removed on clean exit.

## Wire protocol

Messages are length-prefixed msgpack:

```
[4 bytes: big-endian uint32 length][msgpack payload]
```

The msgpack payload is always an array. The first element is the message type (integer), followed by type-specific fields.

## Messages: server to app

These are sent by tmtv to your connected program.

### USER_LIST (3)

```
[3, [{id: int, name: string, readonly: bool, type: string}, ...]]
```

Sent immediately after you subscribe. Contains all currently connected viewers. `type` is `"ssh"` or `"web"`. `readonly` is `true` for read-only token connections.

### USER_JOIN (0)

```
[0, user_id: int, name: string, readonly: bool, type: string]
```

A viewer connected to the session.

### USER_LEAVE (1)

```
[1, user_id: int]
```

A viewer disconnected.

### USER_INPUT (2)

```
[2, user_id: int, pane_id: int, keycode: uint64]
```

A keystroke from a specific viewer. `keycode` is a tmux `key_code` value (see tmux source `tmux.h`). `pane_id` is `-1` when the viewer is typing into the active pane.

## Messages: app to server

Your program sends these to tmtv.

### SUBSCRIBE (0)

```
[0]
```

Start receiving events. tmtv responds with a USER_LIST, then streams USER_JOIN, USER_LEAVE, and USER_INPUT as they occur. You must subscribe before you receive anything.

### SET_MIRROR (1)

```
[1, mirror: bool]
```

Controls whether viewer keystrokes reach the terminal PTY:

- **`mirror=true`** (default): keys go to both the terminal and your app. Viewers see their typing echoed normally.
- **`mirror=false`**: keys go only to your app. The terminal does not receive them. Use this when your program handles all input (games, menus, chat input).

## Example: Python

Minimal client using only `msgpack` (install: `pip install msgpack`).

```python
#!/usr/bin/env python3
import os, socket, struct, msgpack

sock_path = os.environ["TMTV_INPUT_SOCKET"]

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)

# Subscribe
payload = msgpack.packb([0])
s.sendall(struct.pack(">I", len(payload)) + payload)

# Disable mirror — capture keys exclusively
payload = msgpack.packb([1, False])
s.sendall(struct.pack(">I", len(payload)) + payload)

def recv_msg(s):
    header = s.recv(4)
    if len(header) < 4:
        return None
    length = struct.unpack(">I", header)[0]
    data = b""
    while len(data) < length:
        chunk = s.recv(length - len(data))
        if not chunk:
            return None
        data += chunk
    return msgpack.unpackb(data)

# Event loop
while True:
    msg = recv_msg(s)
    if msg is None:
        break
    msg_type = msg[0]
    if msg_type == 3:  # USER_LIST
        print(f"Connected viewers: {msg[1]}")
    elif msg_type == 0:  # USER_JOIN
        print(f"Join: user_id={msg[1]} name={msg[2]} ro={msg[3]} type={msg[4]}")
    elif msg_type == 1:  # USER_LEAVE
        print(f"Leave: user_id={msg[1]}")
    elif msg_type == 2:  # USER_INPUT
        print(f"Input: user_id={msg[1]} pane={msg[2]} key={msg[3]}")
```

## Example: quick test with socat

Verify the socket is working without writing code. This connects and sends a raw subscribe message:

```sh
# Subscribe message: msgpack [0] = 0x91 0x00, length = 2
printf '\x00\x00\x00\x02\x91\x00' | socat - UNIX-CONNECT:$TMTV_INPUT_SOCKET
```

You will see binary msgpack output (the USER_LIST response). Pipe through a msgpack decoder for readable output:

```sh
printf '\x00\x00\x00\x02\x91\x00' | \
  socat - UNIX-CONNECT:$TMTV_INPUT_SOCKET | \
  python3 -c "
import sys, struct, msgpack
data = sys.stdin.buffer.read()
pos = 0
while pos < len(data):
    if pos + 4 > len(data): break
    length = struct.unpack('>I', data[pos:pos+4])[0]
    pos += 4
    if pos + length > len(data): break
    print(msgpack.unpackb(data[pos:pos+length]))
    pos += length
"
```

## Lifecycle

- The socket is created when the tmtv client starts and sets `TMTV_INPUT_SOCKET` in the session environment.
- Multiple programs can connect simultaneously — each gets its own event stream.
- The socket is removed when the tmtv session exits.
- If your program disconnects and reconnects, send SUBSCRIBE again to get a fresh USER_LIST.
- SET_MIRROR state is per-connection. Multiple apps can have different mirror settings.
