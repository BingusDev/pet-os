/*
 * SPDX-License-Identifier: MIT
 */

#include "json.h"

#include <cctype>
#include <cstdlib>

namespace platform::json {

namespace {
const Value& null_value() {
    static const Value kNull;
    return kNull;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
} // namespace

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool parse_document(Value& out) {
        if (!parse_value(out, 0)) return false;
        skip_ws();
        return pos_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    bool parse_hex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(pos_, 2, "\\u") == 0) {
                    pos_ += 2;
                    uint32_t low = 0;
                    if (!parse_hex4(low)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip_ws();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            out.type_ = Value::Type::Object;
            ++pos_;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (true) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) return false;
                skip_ws();
                if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                ++pos_;
                Value child;
                if (!parse_value(child, depth + 1)) return false;
                out.object_[key] = std::move(child);
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            out.type_ = Value::Type::Array;
            ++pos_;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (true) {
                Value child;
                if (!parse_value(child, depth + 1)) return false;
                out.array_.push_back(std::move(child));
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.type_ = Value::Type::String;
            return parse_string(out.string_);
        }
        if (literal("true")) {
            out.type_ = Value::Type::Bool;
            out.bool_ = true;
            return true;
        }
        if (literal("false")) {
            out.type_ = Value::Type::Bool;
            return true;
        }
        if (literal("null")) {
            return true;
        }
        char* end = nullptr;
        const double number = std::strtod(s_.c_str() + pos_, &end);
        if (end == s_.c_str() + pos_) return false;
        pos_ = static_cast<size_t>(end - s_.c_str());
        out.type_ = Value::Type::Number;
        out.number_ = number;
        return true;
    }

    const std::string& s_;
    size_t pos_{0};
};

const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Object) return null_value();
    const auto it = object_.find(key);
    return it == object_.end() ? null_value() : it->second;
}

const Value& Value::operator[](size_t index) const {
    return type_ == Type::Array && index < array_.size() ? array_[index] : null_value();
}

size_t Value::size() const {
    return type_ == Type::Array ? array_.size() : type_ == Type::Object ? object_.size() : 0;
}

std::string Value::str(const std::string& fallback) const {
    if (type_ == Type::String) return string_;
    if (type_ == Type::Number) {
        const long long whole = static_cast<long long>(number_);
        return static_cast<double>(whole) == number_ ? std::to_string(whole) : std::to_string(number_);
    }
    return fallback;
}

double Value::num(double fallback) const {
    return type_ == Type::Number ? number_ : fallback;
}

long long Value::integer(long long fallback) const {
    return type_ == Type::Number ? static_cast<long long>(number_) : fallback;
}

bool Value::boolean(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

bool parse(const std::string& text, Value& out) {
    out = Value{};
    Parser parser(text);
    if (!parser.parse_document(out)) {
        out = Value{};
        return false;
    }
    return true;
}

} // namespace platform::json
