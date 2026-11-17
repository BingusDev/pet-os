/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

// Minimal JSON DOM for the HTTP APIs AlicePET reads (Jellyfin, RomM, bridge).
// Numbers are kept as double; strings are UTF-8 with escapes resolved.
namespace platform::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Lookups never throw: a missing key / index yields a shared null value.
    const Value& operator[](const std::string& key) const;
    const Value& operator[](size_t index) const;
    size_t size() const;

    std::string str(const std::string& fallback = "") const;
    double num(double fallback = 0) const;
    long long integer(long long fallback = 0) const;
    bool boolean(bool fallback = false) const;
    const std::vector<Value>& items() const { return array_; }

private:
    friend class Parser;
    Type type_{Type::Null};
    bool bool_{false};
    double number_{0};
    std::string string_;
    std::vector<Value> array_;
    std::map<std::string, Value> object_;
};

// Parses `text`; returns false (and a null value) on malformed input.
bool parse(const std::string& text, Value& out);

} // namespace platform::json
