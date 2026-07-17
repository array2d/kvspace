// kvspace Redis client — hiredis-backed implementation of kvspace::Client.
// Aligned with Go kvspace/redis/redis.go.
#pragma once

#include "kvspace/client.h"
#include <memory>
#include <string>
#include <unordered_map>

struct redisContext;

namespace kvspace {

class RedisClient : public Client {
public:
    // Connect creates a new Redis-backed Client.
    // addr: "127.0.0.1", port: 6379
    static std::unique_ptr<RedisClient> Connect(const std::string& addr, int port);

    ~RedisClient() override;

    // ── Client interface ─────────────────────────────────────────────────────

    XValue Get(const std::string& key) override;
    void   Set(const std::string& key, const XValue& val) override;
    void   Del(const std::vector<std::string>& keys) override;

    std::vector<XValue> GetMany(const std::vector<std::string>& keys) override;
    void SetMany(const std::vector<KVPair>& pairs) override;

    std::vector<std::string> List(const std::string& prefix) override;
    void DelTree(const std::string& prefix) override;

    void  Notify(const std::string& key, const XValue& val) override;
    XValue Watch(const std::string& key, std::chrono::milliseconds timeout) override;

    void Link(const std::string& target, const std::string& linkpath) override;
    void Unlink(const std::string& linkpath) override;

    void Close() override;

private:
    RedisClient() = default;
    bool connect(const std::string& addr, int port);

    // Internal helpers.
    std::string resolve(const std::string& path);
    std::string checkLink(const std::string& path);
    void maintainIndex(const std::string& key, bool add);
    void delRecursive(const std::string& prefix);

    redisContext* rdb_ = nullptr;

    // Soft link cache (same lazy-check pattern as Go).
    struct LinkEntry { bool checked = false; std::string target; };
    std::unordered_map<std::string, LinkEntry> links_;
    // In a real multi-threaded implementation, protect with mutex.
};

} // namespace kvspace
