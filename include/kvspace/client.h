// kvspace::Client — abstract KV storage interface.
// Aligned with Go kvspace.KVSpace interface.
#pragma once

#include "kvspace/xvalue.h"
#include <chrono>
#include <string>
#include <vector>

namespace kvspace {

// KVPair for batch SetMany.
struct KVPair {
    std::string key;
    XValue      val;
};

// Client is the abstract KV storage interface.
// Implementations: RedisClient (hiredis).
//
// Usage:
//   auto kv = kvspace::RedisClient::Connect("127.0.0.1", 6379);
//   kv.Set("/vt/0/pc", XValue::Str("init/[0,0]"));
//   XValue v = kv.Get("/vt/0/pc");
//   kv.Notify("/vt/0/cmd", XValue::Str("run"));
//   XValue resp = kv.Watch("/vt/0/done", std::chrono::seconds(5));
class Client {
public:
    virtual ~Client() = default;

    // ── single key CRUD ─────────────────────────────────────────────────────

    // Get returns the value at key. Throws ErrNotFound if key does not exist.
    virtual XValue Get(const std::string& key) = 0;

    // Set writes val to key, maintaining directory index.
    virtual void Set(const std::string& key, const XValue& val) = 0;

    // Del removes one or more keys exactly.
    virtual void Del(const std::vector<std::string>& keys) = 0;
    void Del(const std::string& key) { Del(std::vector<std::string>{key}); }

    // ── batch ────────────────────────────────────────────────────────────────

    // GetMany returns values for keys. Missing keys → Null().
    virtual std::vector<XValue> GetMany(const std::vector<std::string>& keys) = 0;

    // SetMany batch-writes pairs.
    virtual void SetMany(const std::vector<KVPair>& pairs) = 0;

    // ── directory ────────────────────────────────────────────────────────────

    // List returns direct child names under prefix.
    virtual std::vector<std::string> List(const std::string& prefix) = 0;

    // DelTree recursively deletes prefix and all children.
    virtual void DelTree(const std::string& prefix) = 0;

    // ── notification ─────────────────────────────────────────────────────────

    // Notify pushes val to key's notification queue (LPUSH).
    virtual void Notify(const std::string& key, const XValue& val) = 0;

    // Watch blocks until a notification arrives at key (BLPOP).
    // Returns ErrNotFound on timeout.
    virtual XValue Watch(const std::string& key, std::chrono::milliseconds timeout) = 0;

    // ── soft links ───────────────────────────────────────────────────────────

    // Link creates a soft link: linkpath → target.
    virtual void Link(const std::string& target, const std::string& linkpath) = 0;

    // Unlink removes a link (does not affect target).
    virtual void Unlink(const std::string& linkpath) = 0;

    // ── lifecycle ────────────────────────────────────────────────────────────

    virtual void Close() = 0;
};

} // namespace kvspace
