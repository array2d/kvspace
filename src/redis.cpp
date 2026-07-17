// kvspace Redis client implementation. Aligned with Go kvspace/redis/redis.go.
#include "kvspace/redis.h"
#include "kvspace/errors.h"

extern "C" {
#include <hiredis/hiredis.h>
}

#include <cstring>
#include <sstream>
#include <unordered_map>

namespace kvspace {

// ── connection ───────────────────────────────────────────────────────────────

std::unique_ptr<RedisClient> RedisClient::Connect(const std::string& addr, int port) {
    auto c = std::unique_ptr<RedisClient>(new RedisClient());
    if (!c->connect(addr, port)) return nullptr;
    return c;
}

bool RedisClient::connect(const std::string& addr, int port) {
    struct timeval tv = {2, 0};
    rdb_ = redisConnectWithTimeout(addr.c_str(), port, tv);
    return rdb_ != nullptr && rdb_->err == 0;
}

RedisClient::~RedisClient() { Close(); }

void RedisClient::Close() {
    if (rdb_) { redisFree(rdb_); rdb_ = nullptr; }
}

// ── helper: run command, check reply ─────────────────────────────────────────

namespace {

// Free reply with RAII.
struct ReplyGuard {
    redisReply* r;
    ~ReplyGuard() { if (r) freeReplyObject(r); }
};

// Execute a Redis command. Returns reply; caller checks type.
redisReply* cmd(redisContext* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    redisReply* r = (redisReply*)redisvCommand(c, fmt, ap);
    va_end(ap);
    return r;
}

// Throw if reply indicates error.
void checkReply(redisReply* r, const std::string& op) {
    if (!r) throw Error("kvspace: " + op + ": null reply (connection lost?)");
    if (r->type == REDIS_REPLY_ERROR)
        throw Error("kvspace: " + op + ": " + std::string(r->str));
}

// ── soft links ───────────────────────────────────────────────────────────────

const std::string LINK_SENTINEL = "->"; // Go: linkSentinel = "->"

} // anonymous namespace

std::string RedisClient::checkLink(const std::string& path) {
    auto& e = links_[path];
    if (e.checked) return e.target; // cached

    ReplyGuard g{cmd(rdb_, "GET %s", path.c_str())};
    if (g.r && g.r->type == REDIS_REPLY_STRING) {
        std::string val(g.r->str, g.r->len);
        if (val.size() >= 2 && val[0] == '-' && val[1] == '>') {
            e.target = val.substr(2);
        }
    }
    e.checked = true;
    return e.target;
}

// resolve resolves soft links from shortest to longest prefix.
// Aligned with Go kvspace.ResolveCore.
std::string RedisClient::resolve(const std::string& path) {
    std::string result = path;
    for (int hop = 0; hop < 40; hop++) {
        bool found = false;
        for (size_t i = 1; i < result.size(); ) {
            size_t j = result.find('/', i);
            if (j == std::string::npos) break;
            i = j;
            std::string prefix = result.substr(0, i);
            std::string target = checkLink(prefix);
            if (!target.empty()) {
                result = target + result.substr(i);
                found = true;
                break;
            }
            i++;
        }
        if (!found) {
            std::string target = checkLink(result);
            if (!target.empty()) {
                result = target;
                found = true;
            }
        }
        if (!found) return result;
    }
    throw ErrLinkLoop();
}

// ── directory index maintenance ──────────────────────────────────────────────

void RedisClient::maintainIndex(const std::string& key, bool add) {
    std::string prefix;
    size_t start = 1; // skip leading '/'
    while (start < key.size()) {
        size_t end = key.find('/', start);
        std::string part = (end == std::string::npos) ? key.substr(start) : key.substr(start, end - start);
        if (part.empty() || part == ".") break;

        std::string parent = prefix.empty() ? "/" : prefix;
        ReplyGuard g{cmd(rdb_, add ? "SADD %s/. %s" : "SREM %s/. %s",
                          parent.c_str(), part.c_str())};
        prefix += "/" + part;
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void RedisClient::delRecursive(const std::string& prefix) {
    // List children
    ReplyGuard children{cmd(rdb_, "SMEMBERS %s/.", prefix.c_str())};
    if (children.r && children.r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < children.r->elements; i++) {
            std::string child(children.r->element[i]->str);
            delRecursive(prefix + "/" + child);
        }
    }
    ReplyGuard g{cmd(rdb_, "DEL %s %s/.", prefix.c_str(), prefix.c_str())};
}

// ── CRUD ─────────────────────────────────────────────────────────────────────

XValue RedisClient::Get(const std::string& key) {
    std::string resolved = resolve(key);
    ReplyGuard g{cmd(rdb_, "GET %s", resolved.c_str())};
    if (!g.r || g.r->type == REDIS_REPLY_NIL)
        throw ErrNotFound(key);
    checkReply(g.r, "Get");
    return XValue::Decode(reinterpret_cast<const uint8_t*>(g.r->str), g.r->len);
}

void RedisClient::Set(const std::string& key, const XValue& val) {
    std::string resolved = resolve(key);
    maintainIndex(resolved, true);
    auto encoded = val.Encode();
    ReplyGuard g{cmd(rdb_, "SET %s %b", resolved.c_str(), encoded.data(), encoded.size())};
    checkReply(g.r, "Set");
}

void RedisClient::Del(const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        std::string resolved = resolve(k);
        maintainIndex(resolved, false);
        ReplyGuard g{cmd(rdb_, "DEL %s", resolved.c_str())};
    }
}

std::vector<XValue> RedisClient::GetMany(const std::vector<std::string>& keys) {
    if (keys.empty()) return {};
    // Build MGET command with resolved keys
    std::ostringstream oss;
    oss << "MGET";
    for (const auto& k : keys) {
        oss << " " << resolve(k);
    }
    // hiredis doesn't have a clean variadic MGET, build argc/argv manually
    int argc = 1 + (int)keys.size();
    std::vector<const char*> argv(argc);
    std::vector<size_t> argvlen(argc);
    argv[0] = "MGET"; argvlen[0] = 4;
    for (size_t i = 0; i < keys.size(); i++) {
        std::string resolved = resolve(keys[i]);
        char* s = new char[resolved.size()];
        std::memcpy(s, resolved.data(), resolved.size());
        argv[i+1] = s;
        argvlen[i+1] = resolved.size();
    }
    ReplyGuard g{reinterpret_cast<redisReply*>(
        redisCommandArgv(rdb_, argc, argv.data(), argvlen.data()))};
    for (size_t i = 1; i < argv.size(); i++) delete[] argv[i];

    std::vector<XValue> results(keys.size());
    if (g.r && g.r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < g.r->elements && i < keys.size(); i++) {
            if (g.r->element[i]->type != REDIS_REPLY_NIL) {
                results[i] = XValue::Decode(
                    reinterpret_cast<const uint8_t*>(g.r->element[i]->str),
                    g.r->element[i]->len);
            }
        }
    }
    return results;
}

void RedisClient::SetMany(const std::vector<KVPair>& pairs) {
    if (pairs.empty()) return;
    for (const auto& p : pairs) {
        Set(p.key, p.val); // Individual SET with index maintenance
    }
    // TODO: pipeline for efficiency (matching Go pipeline approach)
}

std::vector<std::string> RedisClient::List(const std::string& prefix) {
    std::string resolved = resolve(prefix);
    ReplyGuard g{cmd(rdb_, "SMEMBERS %s/.", resolved.c_str())};
    std::vector<std::string> result;
    if (g.r && g.r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < g.r->elements; i++) {
            result.emplace_back(g.r->element[i]->str);
        }
    }
    return result;
}

void RedisClient::DelTree(const std::string& prefix) {
    if (!checkLink(prefix).empty()) {
        Unlink(prefix); // link → just remove link, not target tree
        return;
    }
    std::string resolved = resolve(prefix);
    delRecursive(resolved);
    maintainIndex(resolved, false);
}

// ── notification ─────────────────────────────────────────────────────────────

void RedisClient::Notify(const std::string& key, const XValue& val) {
    std::string resolved = resolve(key);
    auto encoded = val.Encode();
    ReplyGuard g{cmd(rdb_, "LPUSH %s %b", resolved.c_str(), encoded.data(), encoded.size())};
    checkReply(g.r, "Notify");
}

XValue RedisClient::Watch(const std::string& key, std::chrono::milliseconds timeout) {
    std::string resolved = resolve(key);
    int secs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(timeout).count());
    ReplyGuard g{cmd(rdb_, "BLPOP %s %d", resolved.c_str(), secs)};
    if (!g.r || g.r->type == REDIS_REPLY_NIL)
        throw ErrNotFound(key);
    checkReply(g.r, "Watch");
    // BLPOP returns [key, value]
    if (g.r->type != REDIS_REPLY_ARRAY || g.r->elements < 2)
        throw ErrNotFound(key);
    return XValue::Decode(
        reinterpret_cast<const uint8_t*>(g.r->element[1]->str),
        g.r->element[1]->len);
}

// ── soft links ───────────────────────────────────────────────────────────────

void RedisClient::Link(const std::string& target, const std::string& linkpath) {
    std::string val = LINK_SENTINEL + target;
    ReplyGuard g{cmd(rdb_, "SET %s %b", linkpath.c_str(), val.data(), val.size())};
    checkReply(g.r, "Link");
    maintainIndex(linkpath, true);
    links_[linkpath] = {true, target};
}

void RedisClient::Unlink(const std::string& linkpath) {
    ReplyGuard g{cmd(rdb_, "DEL %s", linkpath.c_str())};
    checkReply(g.r, "Unlink");
    maintainIndex(linkpath, false);
    links_[linkpath] = {true, ""};
}

} // namespace kvspace
