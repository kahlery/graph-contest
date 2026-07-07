// Minimal JSON library shared by the contest tooling (orchestrator, batch
// runner, stress-init, server). Extracted from src/main.cpp's embedded
// mjson so the solvers themselves stay untouched.
#pragma once

#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mjson {

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

class Value {
public:
    enum Type { NULL_T, BOOL_T, NUMBER_T, STRING_T, ARRAY_T, OBJECT_T };

    Type        type = NULL_T;
    bool        b    = false;
    double      num  = 0;
    std::string str;
    Array       arr;
    Object      obj;

    Value() = default;
    Value(bool v)               : type(BOOL_T), b(v) {}
    Value(double v)             : type(NUMBER_T), num(v) {}
    Value(int v)                : type(NUMBER_T), num((double)v) {}
    Value(long long v)          : type(NUMBER_T), num((double)v) {}
    Value(size_t v)             : type(NUMBER_T), num((double)v) {}
    Value(const char* v)        : type(STRING_T), str(v) {}
    Value(const std::string& v) : type(STRING_T), str(v) {}

    static Value makeArray()  { Value v; v.type = ARRAY_T;  return v; }
    static Value makeObject() { Value v; v.type = OBJECT_T; return v; }

    bool isObj() const { return type == OBJECT_T; }
    bool isArr() const { return type == ARRAY_T; }
    bool isNum() const { return type == NUMBER_T; }
    bool isStr() const { return type == STRING_T; }
    bool isNull() const { return type == NULL_T; }

    Value& operator[](const std::string& k) { type = OBJECT_T; return obj[k]; }
    const Value& operator[](const std::string& k) const { return obj.at(k); }
    Value& operator[](size_t i) { return arr[i]; }
    const Value& operator[](size_t i) const { return arr[i]; }

    const Value& at(const std::string& k) const { return obj.at(k); }
    Value&       at(const std::string& k)       { return obj.at(k); }

    void push_back(Value v) { type = ARRAY_T; arr.push_back(std::move(v)); }

    int         asInt()    const { return (int)num; }
    long long   asLL()     const { return (long long)num; }
    double      asDouble() const { return num; }
    bool        asBool()   const { return b; }
    const std::string& asString() const { return str; }
    const Array&  asArray()  const { return arr; }
    const Object& asObject() const { return obj; }

    bool has(const std::string& k) const {
        return type == OBJECT_T && obj.find(k) != obj.end();
    }

    // Convenience getters with a default when the key is absent.
    long long getLL(const std::string& k, long long def) const {
        return has(k) ? at(k).asLL() : def;
    }
    double getDouble(const std::string& k, double def) const {
        return has(k) ? at(k).asDouble() : def;
    }
    std::string getStr(const std::string& k, const std::string& def) const {
        return has(k) ? at(k).asString() : def;
    }
    bool getBool(const std::string& k, bool def) const {
        return has(k) ? at(k).asBool() : def;
    }

    void serialize(std::ostream& os, int indent = 0, int depth = 0) const;
    std::string dump(int indent = 0) const {
        std::ostringstream os;
        serialize(os, indent, 0);
        return os.str();
    }
};

class Parser {
    const char* p;
    const char* end;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    void expect(char c) {
        skipWs();
        if (p >= end || *p != c)
            throw std::runtime_error(std::string("JSON expected ") + c);
        p++;
    }
    bool peek(char c) { skipWs(); return p < end && *p == c; }

    std::string parseString() {
        expect('"');
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                char c = *p++;
                switch (c) {
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case 'r':  s += '\r'; break;
                    case '\\': s += '\\'; break;
                    case '"':  s += '"';  break;
                    case '/':  s += '/';  break;
                    default:   s += c;
                }
            } else {
                s += *p++;
            }
        }
        expect('"');
        return s;
    }
    double parseNumber() {
        skipWs();
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) p++;
        while (p < end && (isdigit((unsigned char)*p) || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++;
        return std::stod(std::string(start, p - start));
    }

public:
    Parser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    Value parse() {
        skipWs();
        if (p >= end) throw std::runtime_error("JSON empty input");
        return parseValue();
    }

    Value parseValue() {
        skipWs();
        if (p >= end) throw std::runtime_error("JSON unexpected EOF");
        if (*p == '{') return parseObject();
        if (*p == '[') return parseArray();
        if (*p == '"') {
            Value v; v.type = Value::STRING_T; v.str = parseString(); return v;
        }
        if (*p == 't') { p += 4; Value v; v.type = Value::BOOL_T; v.b = true;  return v; }
        if (*p == 'f') { p += 5; Value v; v.type = Value::BOOL_T; v.b = false; return v; }
        if (*p == 'n') { p += 4; return Value(); }
        Value v; v.type = Value::NUMBER_T; v.num = parseNumber(); return v;
    }

    Value parseObject() {
        expect('{');
        Value v; v.type = Value::OBJECT_T;
        if (peek('}')) { p++; return v; }
        while (true) {
            std::string key = parseString();
            expect(':');
            v.obj[key] = parseValue();
            skipWs();
            if (peek(',')) { p++; continue; }
            break;
        }
        expect('}');
        return v;
    }

    Value parseArray() {
        expect('[');
        Value v; v.type = Value::ARRAY_T;
        if (peek(']')) { p++; return v; }
        while (true) {
            v.arr.push_back(parseValue());
            skipWs();
            if (peek(',')) { p++; continue; }
            break;
        }
        expect(']');
        return v;
    }
};

inline Value parse(const std::string& s) { return Parser(s).parse(); }

inline std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

inline Value parseFile(const std::string& path) { return parse(slurp(path)); }

inline void writeFile(const std::string& path, const Value& v, int indent = 2) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write " + path);
    v.serialize(f, indent);
    f << '\n';
}

inline void Value::serialize(std::ostream& os, int indent, int depth) const {
    auto ind = [&](int d) { for (int i = 0; i < d * indent; i++) os << ' '; };
    switch (type) {
        case NULL_T:   os << "null"; break;
        case BOOL_T:   os << (b ? "true" : "false"); break;
        case NUMBER_T:
            if (std::isfinite(num) && num == (double)(long long)num)
                os << (long long)num;
            else
                os << num;
            break;
        case STRING_T:
            os << '"';
            for (char c : str) {
                if      (c == '"')  os << "\\\"";
                else if (c == '\\') os << "\\\\";
                else if (c == '\n') os << "\\n";
                else if (c == '\t') os << "\\t";
                else                os << c;
            }
            os << '"';
            break;
        case ARRAY_T: {
            os << '[';
            if (indent && !arr.empty()) os << '\n';
            for (size_t i = 0; i < arr.size(); i++) {
                if (indent) ind(depth + 1);
                arr[i].serialize(os, indent, depth + 1);
                if (i + 1 < arr.size()) os << ',';
                if (indent) os << '\n';
            }
            if (indent && !arr.empty()) ind(depth);
            os << ']';
            break;
        }
        case OBJECT_T: {
            os << '{';
            if (indent && !obj.empty()) os << '\n';
            size_t i = 0;
            for (const auto& kv : obj) {
                if (indent) ind(depth + 1);
                os << '"' << kv.first << "\":";
                if (indent) os << ' ';
                kv.second.serialize(os, indent, depth + 1);
                if (++i < obj.size()) os << ',';
                if (indent) os << '\n';
            }
            if (indent && !obj.empty()) ind(depth);
            os << '}';
            break;
        }
    }
}

} // namespace mjson
