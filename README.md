# kvspace-cpp

C++ kvspace client — hiredis-backed implementation aligned with Go `kvspace.KVSpace`.

## API

```cpp
#include "kvspace/redis.h"

auto kv = kvspace::RedisClient::Connect("127.0.0.1", 6379);

// CRUD
kv->Set("/key", kvspace::XValue::Int(42));
kvspace::XValue v = kv->Get("/key");          // v.AsInt() == 42
kv->Del("/key");

// Directory
kv->Set("/a/x", kvspace::XValue::Str("hi"));
kv->Set("/a/y", kvspace::XValue::Str("yo"));
auto children = kv->List("/a");               // {"x", "y"}
kv->DelTree("/a");                            // delete /a recursively

// Notification (LPUSH / BLPOP)
kv->Notify("/queue", kvspace::XValue::Str("task"));
kvspace::XValue msg = kv->Watch("/queue", std::chrono::seconds(5));

// Soft links
kv->Link("/target", "/link");                 // /link → /target
kvspace::XValue v2 = kv->Get("/link/x");      // resolves to /target/x
kv->Unlink("/link");
```

## XValue Types

| Factory | Go equivalent | C++ type |
|---------|--------------|----------|
| `XValue::Int(v)` | `kvspace.Int(v)` | `int64_t` |
| `XValue::Float(v)` | `kvspace.Float(v)` | `double` |
| `XValue::Bool(v)` | `kvspace.Bool(v)` | `bool` |
| `XValue::Str(v)` | `kvspace.Str(v)` | `std::string_view` |
| `XValue::Bytes(v)` | `kvspace.Bytes(v)` | `std::vector<uint8_t>` |
| `XValue::Raw(kind, raw)` | `kvspace.Raw(kind, raw)` | arbitrary |

TLV wire format (identical to Go):
```
[1B kind_len][N B kind_name][4B raw_len LE][M B raw_value]
```

## Build

```bash
mkdir build && cd build
cmake .. && make
```

## Dependencies

- [hiredis](https://github.com/redis/hiredis) — C Redis client
- C++17
