# Building on TMTV_INPUT_SOCKET

Everything an AI agent or developer needs to implement a client library for the tmtv per-user input socket in any language.

## Protocol at a glance

```
Transport:  Unix domain socket (SOCK_STREAM)
Discovery:  $TMTV_INPUT_SOCKET environment variable
Framing:    4-byte big-endian length prefix + msgpack payload
Direction:  Full duplex — both sides send length-prefixed msgpack arrays
```

## Byte-level framing

Every message on the wire looks like this:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Payload length (uint32 BE)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Msgpack payload (variable)                  |
|                           ...                                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

The msgpack payload is always an **array**. Element 0 is the message type (integer). Remaining elements are type-specific.

Read loop pseudocode:

```
loop:
    read exactly 4 bytes → length
    read exactly `length` bytes → payload
    decode payload as msgpack array
    dispatch on payload[0]
```

## Message type reference

### Server → App

| Type | ID | Fields | Description |
|------|-----|--------|-------------|
| USER_JOIN | 0 | `[0, user_id:int, name:str, readonly:bool, type:str]` | Viewer connected |
| USER_LEAVE | 1 | `[1, user_id:int]` | Viewer disconnected |
| USER_INPUT | 2 | `[2, user_id:int, pane_id:int, keycode:uint64]` | Keystroke from viewer |
| USER_LIST | 3 | `[3, [{id:int, name:str, readonly:bool, type:str}, ...]]` | All current viewers (response to SUBSCRIBE) |

### App → Server

| Type | ID | Fields | Description |
|------|-----|--------|-------------|
| SUBSCRIBE | 0 | `[0]` | Start receiving events |
| SET_MIRROR | 1 | `[1, mirror:bool]` | Control whether keys reach the terminal PTY |

## Field semantics

**`user_id`** — Stable integer identifying a viewer within the session. SSH viewers use their process ID. Web viewers get a monotonically increasing counter. IDs are unique within a session but not across sessions.

**`name`** — Human-readable label. SSH viewers: `"ssh-<pid>"`. Web viewers: `"web"` or `"web-ro"`. Your app should treat this as a display name, not an identifier — use `user_id` for identity.

**`readonly`** — `true` if the viewer connected with a read-only token. Read-only viewers can still have their keystrokes captured when `mirror=false`.

**`type`** — Either `"ssh"` or `"web"`. Use this to distinguish connection method.

**`pane_id`** — The tmux pane where the key was typed. `-1` means the active pane. For most apps, ignore this — you care about *who* typed, not *where*.

**`keycode`** — A tmux `key_code` value (64-bit unsigned). For printable ASCII, this is the character value (e.g., `0x61` = `'a'`). For special keys, see the constants below.

## Key code mapping

Printable ASCII (0x20–0x7E) maps directly: `keycode == character`.

Common special keys (from tmux `tmux.h`):

```
Enter       = 0x0D  (13)
Tab         = 0x09  (9)
Escape      = 0x1B  (27)
Backspace   = 0x7F  (127)
Space       = 0x20  (32)

Ctrl+A      = 0x200000000061  (KEYC_CTRL | 'a')
Ctrl+B      = 0x200000000062  (KEYC_CTRL | 'b')
...
Ctrl+Z      = 0x20000000007A  (KEYC_CTRL | 'z')

KEYC_CTRL   = 0x200000000000
KEYC_META   = 0x400000000000  (Alt key)
KEYC_SHIFT  = 0x100000000000
```

For a game or chat app, you typically only need printable ASCII + Enter + Backspace + arrow keys. Ignore keycodes you don't recognize.

Practical approach — convert to character:

```python
def keycode_to_char(keycode):
    """Convert tmux keycode to a printable character, or None."""
    key = keycode & 0x000FFFFFFFFFFF  # strip modifiers
    if 0x20 <= key <= 0x7E:
        return chr(key)
    if key == 0x0D:
        return '\n'
    return None
```

## Connection lifecycle

```
1. Read $TMTV_INPUT_SOCKET
2. Connect to Unix socket (SOCK_STREAM)
3. Send SUBSCRIBE: [0]
4. Receive USER_LIST: [3, [...]]
5. Optionally send SET_MIRROR: [1, false]  (capture keys exclusively)
6. Loop: receive USER_JOIN / USER_LEAVE / USER_INPUT
7. On disconnect: socket closes, reconnect and re-SUBSCRIBE if needed
```

## Implementation template

This is language-agnostic pseudocode. Implement `send_msg`, `recv_msg`, and the event loop in your language of choice.

```
function send_msg(socket, payload):
    packed = msgpack_encode(payload)
    socket.write(uint32_be(len(packed)))
    socket.write(packed)

function recv_msg(socket):
    header = socket.read_exact(4)
    length = uint32_be_decode(header)
    data = socket.read_exact(length)
    return msgpack_decode(data)

function main():
    path = env("TMTV_INPUT_SOCKET")
    if not path:
        print("Not running inside a tmtv session")
        exit(1)

    sock = unix_connect(path)
    send_msg(sock, [0])          # SUBSCRIBE
    send_msg(sock, [1, false])   # SET_MIRROR off — exclusive input

    users = {}

    while true:
        msg = recv_msg(sock)
        if msg is null:
            break

        match msg[0]:
            case 3:  # USER_LIST
                for user in msg[1]:
                    users[user.id] = user
                on_user_list(users)

            case 0:  # USER_JOIN
                users[msg[1]] = {id: msg[1], name: msg[2], readonly: msg[3], type: msg[4]}
                on_user_join(users[msg[1]])

            case 1:  # USER_LEAVE
                user = users.pop(msg[1])
                on_user_leave(user)

            case 2:  # USER_INPUT
                char = keycode_to_char(msg[3])
                if char:
                    on_user_input(msg[1], char)
```

## Go implementation sketch

```go
package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"

	"github.com/vmihailenco/msgpack/v5"
)

func sendMsg(conn net.Conn, payload interface{}) error {
	data, err := msgpack.Marshal(payload)
	if err != nil {
		return err
	}
	header := make([]byte, 4)
	binary.BigEndian.PutUint32(header, uint32(len(data)))
	if _, err := conn.Write(header); err != nil {
		return err
	}
	_, err = conn.Write(data)
	return err
}

func recvMsg(conn net.Conn) ([]interface{}, error) {
	header := make([]byte, 4)
	if _, err := io.ReadFull(conn, header); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(header)
	data := make([]byte, length)
	if _, err := io.ReadFull(conn, data); err != nil {
		return nil, err
	}
	var msg []interface{}
	if err := msgpack.Unmarshal(data, &msg); err != nil {
		return nil, err
	}
	return msg, nil
}

func main() {
	path := os.Getenv("TMTV_INPUT_SOCKET")
	if path == "" {
		fmt.Fprintln(os.Stderr, "not in a tmtv session")
		os.Exit(1)
	}

	conn, err := net.Dial("unix", path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "connect: %v\n", err)
		os.Exit(1)
	}
	defer conn.Close()

	sendMsg(conn, []interface{}{0})        // SUBSCRIBE
	sendMsg(conn, []interface{}{1, false}) // SET_MIRROR off

	for {
		msg, err := recvMsg(conn)
		if err != nil {
			break
		}
		msgType, _ := msg[0].(int64)
		switch msgType {
		case 3: // USER_LIST
			fmt.Printf("viewers: %v\n", msg[1])
		case 0: // USER_JOIN
			fmt.Printf("join: id=%v name=%v\n", msg[1], msg[2])
		case 1: // USER_LEAVE
			fmt.Printf("leave: id=%v\n", msg[1])
		case 2: // USER_INPUT
			key, _ := msg[3].(uint64)
			if key >= 0x20 && key <= 0x7E {
				fmt.Printf("input: user=%v char=%c\n", msg[1], rune(key))
			}
		}
	}
}
```

## Rust implementation sketch

```rust
use std::env;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;

fn send_msg(stream: &mut UnixStream, payload: &rmp_serde::Value) {
    let data = rmp_serde::to_vec(payload).unwrap();
    let len = (data.len() as u32).to_be_bytes();
    stream.write_all(&len).unwrap();
    stream.write_all(&data).unwrap();
}

fn recv_msg(stream: &mut UnixStream) -> Option<Vec<rmpv::Value>> {
    let mut header = [0u8; 4];
    stream.read_exact(&mut header).ok()?;
    let length = u32::from_be_bytes(header) as usize;
    let mut data = vec![0u8; length];
    stream.read_exact(&mut data).ok()?;
    rmpv::decode::read_value(&mut &data[..]).ok().and_then(|v| {
        if let rmpv::Value::Array(arr) = v { Some(arr) } else { None }
    })
}
```

## Testing your implementation

1. Start a tmtv session: `tmtv new -s test`
2. In the session, run your program
3. Open a second terminal and connect as a viewer: `ssh <token>@tmtv.se`
4. Your program should print USER_JOIN
5. Type in the viewer terminal — your program should print USER_INPUT
6. Disconnect the viewer — your program should print USER_LEAVE

## Common mistakes

- **Forgetting to SUBSCRIBE.** You won't receive any events until you send `[0]`.
- **Not reading the full length-prefixed frame.** TCP/Unix sockets are streams — a single `read()` may return partial data. Always `read_exact`.
- **Treating `user_id` as stable across reconnects.** If a viewer disconnects and reconnects, they get a new `user_id`.
- **Ignoring `mirror` mode.** If you want exclusive input (game, chat), set `mirror=false`. Otherwise viewers see their typing echoed in the terminal AND your app gets it — which is confusing for games.
- **Hardcoding the socket path.** Always read `$TMTV_INPUT_SOCKET`. The path includes the PID and changes every session.
