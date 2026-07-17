// kvspace error codes. Aligned with Go kvspace/errors.go.
#pragma once

#include <stdexcept>
#include <string>

namespace kvspace {

// Error is the base exception for all kvspace operations.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& msg) : std::runtime_error(msg) {}
};

// ErrNotFound: key not found (Get) or Watch timeout.
class ErrNotFound : public Error {
public:
    ErrNotFound() : Error("kvspace: key not found") {}
    explicit ErrNotFound(const std::string& key)
        : Error("kvspace: key not found: " + key) {}
};

// ErrClosed: operation on a closed connection.
class ErrClosed : public Error {
public:
    ErrClosed() : Error("kvspace: connection closed") {}
};

// ErrLinkLoop: soft link resolution exceeded max hops.
class ErrLinkLoop : public Error {
public:
    ErrLinkLoop() : Error("kvspace: link loop detected") {}
};

} // namespace kvspace
