// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// A small JSON value, parser and writer for the lobby protocol (net/lobby.cpp): objects, arrays, strings, numbers, true, false, null. No
// comments, no trailing commas; numbers are doubles. Parsing returns false on any error, leaving the value null.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace json
{
    struct Value
    {
        enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
        bool b = false;
        double n = 0.0;
        std::string s;
        std::vector<Value> a;
        std::map<std::string, Value> o;

        Value() = default;
        Value(bool v) : type(Type::Bool), b(v) {}
        Value(int v) : type(Type::Number), n(v) {}
        Value(double v) : type(Type::Number), n(v) {}
        Value(const char* v) : type(Type::String), s(v) {}
        Value(const std::string& v) : type(Type::String), s(v) {}
        static Value Object() { Value v; v.type = Type::Object; return v; }
        static Value Array() { Value v; v.type = Type::Array; return v; }

        bool IsObject() const { return type == Type::Object; }
        bool IsArray() const { return type == Type::Array; }
        Value& operator[](const std::string& key) { if (type != Type::Object) { *this = Object(); } return o[key]; }
        const Value* Find(const std::string& key) const
        {
            if (type != Type::Object)
                return nullptr;
            auto it = o.find(key);
            return it == o.end() ? nullptr : &it->second;
        }
        std::string Str(const std::string& key, const std::string& fallback = "") const
        {
            const Value* v = Find(key);
            return v && v->type == Type::String ? v->s : fallback;
        }
        double Num(const std::string& key, double fallback = 0.0) const
        {
            const Value* v = Find(key);
            return v && v->type == Type::Number ? v->n : fallback;
        }
        bool Bool(const std::string& key, bool fallback = false) const
        {
            const Value* v = Find(key);
            return v && v->type == Type::Bool ? v->b : fallback;
        }
        void Push(Value v) { if (type != Type::Array) { *this = Array(); } a.push_back(std::move(v)); }
    };

    namespace detail
    {
        struct Parser
        {
            const char* p;
            const char* end;
            int depth = 0;

            void Space() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
            bool Lit(const char* word)
            {
                size_t n = std::char_traits<char>::length(word);
                if (size_t(end - p) < n || std::string(p, n) != word)
                    return false;
                p += n;
                return true;
            }
            bool String(std::string& out)
            {
                if (p >= end || *p != '"')
                    return false;
                p++;
                out.clear();
                while (p < end && *p != '"')
                {
                    char c = *p++;
                    if (c == '\\')
                    {
                        if (p >= end)
                            return false;
                        char e = *p++;
                        switch (e)
                        {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case '"': case '\\': case '/': out += e; break;
                        case 'u':
                        {
                            if (end - p < 4)
                                return false;
                            unsigned cp = unsigned(std::strtoul(std::string(p, 4).c_str(), nullptr, 16));
                            p += 4;
                            if (cp < 0x80)
                                out += char(cp);
                            else if (cp < 0x800)
                            {
                                out += char(0xC0 | (cp >> 6));
                                out += char(0x80 | (cp & 0x3F));
                            }
                            else
                            {
                                out += char(0xE0 | (cp >> 12));
                                out += char(0x80 | ((cp >> 6) & 0x3F));
                                out += char(0x80 | (cp & 0x3F));
                            }
                            break;
                        }
                        default: return false;
                        }
                    }
                    else
                        out += c;
                }
                if (p >= end)
                    return false;
                p++;
                return true;
            }
            bool ParseValue(Value& v)
            {
                Space();
                if (p >= end || ++depth > 32)
                    return false;
                struct Depth { int& d; ~Depth() { d--; } } guard{ depth };
                switch (*p)
                {
                case '{':
                {
                    p++;
                    v = Value::Object();
                    Space();
                    if (p < end && *p == '}')
                    {
                        p++;
                        return true;
                    }
                    while (true)
                    {
                        Space();
                        std::string key;
                        if (!String(key))
                            return false;
                        Space();
                        if (p >= end || *p++ != ':')
                            return false;
                        if (!ParseValue(v.o[key]))
                            return false;
                        Space();
                        if (p >= end)
                            return false;
                        if (*p == ',')
                        {
                            p++;
                            continue;
                        }
                        return *p++ == '}';
                    }
                }
                case '[':
                {
                    p++;
                    v = Value::Array();
                    Space();
                    if (p < end && *p == ']')
                    {
                        p++;
                        return true;
                    }
                    while (true)
                    {
                        v.a.emplace_back();
                        if (!ParseValue(v.a.back()))
                            return false;
                        Space();
                        if (p >= end)
                            return false;
                        if (*p == ',')
                        {
                            p++;
                            continue;
                        }
                        return *p++ == ']';
                    }
                }
                case '"':
                    v.type = Value::Type::String;
                    return String(v.s);
                case 't': v = Value(true); return Lit("true");
                case 'f': v = Value(false); return Lit("false");
                case 'n': v = Value(); return Lit("null");
                default:
                {
                    char* stop = nullptr;
                    std::string text(p, std::min<size_t>(size_t(end - p), 40));
                    double d = std::strtod(text.c_str(), &stop);
                    if (stop == text.c_str() || !std::isfinite(d))
                        return false;
                    p += stop - text.c_str();
                    v = Value(d);
                    return true;
                }
                }
            }
        };

        inline void Quote(std::string& out, const std::string& s)
        {
            out += '"';
            for (unsigned char c : s)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                        out += char(c);
                }
            }
            out += '"';
        }
    }

    inline bool Parse(const char* data, size_t size, Value& out)
    {
        detail::Parser parser{ data, data + size };
        out = Value();
        if (!parser.ParseValue(out))
        {
            out = Value();
            return false;
        }
        parser.Space();
        if (parser.p != parser.end)
        {
            out = Value();
            return false;
        }
        return true;
    }

    inline std::string Write(const Value& v)
    {
        std::string out;
        switch (v.type)
        {
        case Value::Type::Null: out = "null"; break;
        case Value::Type::Bool: out = v.b ? "true" : "false"; break;
        case Value::Type::Number:
        {
            char buf[40];
            if (v.n == std::floor(v.n) && std::fabs(v.n) < 1e15)
                snprintf(buf, sizeof(buf), "%lld", (long long)v.n);
            else
                snprintf(buf, sizeof(buf), "%.9g", v.n);
            out = buf;
            break;
        }
        case Value::Type::String: detail::Quote(out, v.s); break;
        case Value::Type::Array:
            out = "[";
            for (size_t i = 0; i < v.a.size(); i++)
                out += (i ? "," : "") + Write(v.a[i]);
            out += "]";
            break;
        case Value::Type::Object:
        {
            out = "{";
            bool first = true;
            for (const auto& [k, val] : v.o)
            {
                if (!first)
                    out += ",";
                first = false;
                detail::Quote(out, k);
                out += ":" + Write(val);
            }
            out += "}";
            break;
        }
        }
        return out;
    }
}
