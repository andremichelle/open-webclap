// A small JSON reader for the settings the page sends (rare, a few hundred bytes): enough of juce::var for the scope
// engines (plugin/Source/*Engine.h upstream) to read their settings as upstream does, getProperty(key, fallback).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace prism {

class Var {
public:
    enum Type {Null, Bool, Number, String, Array, Object};

    Var() = default;
    Var(bool value) : type(Bool), number(value ? 1.0 : 0.0) {}
    Var(double value) : type(Number), number(value) {}
    Var(int value) : type(Number), number(value) {}
    Var(const char *value) : type(String), text(value) {}
    Var(std::string value) : type(String), text(std::move(value)) {}

    static Var parse(const char *json, size_t length) {
        Parser parser{json, json + length};
        Var result = parser.value();
        return parser.failed ? Var() : result;
    }

    bool isObject() const {return type == Object;}
    bool isNumber() const {return type == Number;}
    bool isString() const {return type == String;}
    bool isBool() const {return type == Bool;}

    // the property, or the fallback when it is missing
    Var getProperty(const char *key, Var fallback) const {
        if (type != Object) {return fallback;}
        const auto found = properties.find(key);
        return found == properties.end() ? fallback : found->second;
    }

    double toDouble() const {return type == Number || type == Bool ? number : type == String ? std::atof(text.c_str()) : 0.0;}
    bool toBool() const {return type == Bool || type == Number ? number != 0.0 : false;}
    std::string toString() const {
        if (type == String) {return text;}
        if (type == Number) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", number);
            return buffer;
        }
        if (type == Bool) {return number != 0.0 ? "true" : "false";}
        return {};
    }

    explicit operator double() const {return toDouble();}
    explicit operator float() const {return static_cast<float>(toDouble());}
    explicit operator int() const {return static_cast<int>(std::lround(toDouble()));}
    explicit operator bool() const {return toBool();}

private:
    struct Parser {
        const char *at;
        const char *end;
        bool failed = false;

        void space() {while (at < end && (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t')) {at++;}}

        bool literal(const char *word) {
            const size_t length = std::strlen(word);
            if (static_cast<size_t>(end - at) < length || std::strncmp(at, word, length) != 0) {return false;}
            at += length;
            return true;
        }

        std::string string() {
            std::string result;
            at++; // the opening quote
            while (at < end && *at != '"') {
                if (*at == '\\' && at + 1 < end) {
                    at++;
                    switch (*at) {
                        case 'n': result += '\n'; break;
                        case 't': result += '\t'; break;
                        case 'r': result += '\r'; break;
                        case 'b': result += '\b'; break;
                        case 'f': result += '\f'; break;
                        case 'u': {
                            // the settings hold no text the engines read beyond ASCII: keep the code as '?'
                            at += std::min<ptrdiff_t>(4, end - at - 1);
                            result += '?';
                            break;
                        }
                        default: result += *at; break;
                    }
                    at++;
                } else {
                    result += *at++;
                }
            }
            if (at >= end) {failed = true; return result;}
            at++; // the closing quote
            return result;
        }

        Var value(int depth = 0) {
            space();
            if (at >= end || depth > 32) {failed = true; return {};}
            if (*at == '{') {
                at++;
                Var object;
                object.type = Object;
                space();
                if (at < end && *at == '}') {at++; return object;}
                while (!failed) {
                    space();
                    if (at >= end || *at != '"') {failed = true; break;}
                    std::string key = string();
                    space();
                    if (at >= end || *at != ':') {failed = true; break;}
                    at++;
                    object.properties[key] = value(depth + 1);
                    space();
                    if (at < end && *at == ',') {at++; continue;}
                    if (at < end && *at == '}') {at++; break;}
                    failed = true;
                }
                return object;
            }
            if (*at == '[') {
                at++;
                Var array;
                array.type = Array;
                space();
                if (at < end && *at == ']') {at++; return array;}
                while (!failed) {
                    array.items.push_back(value(depth + 1));
                    space();
                    if (at < end && *at == ',') {at++; continue;}
                    if (at < end && *at == ']') {at++; break;}
                    failed = true;
                }
                return array;
            }
            if (*at == '"') {return Var(string());}
            if (literal("true")) {return Var(true);}
            if (literal("false")) {return Var(false);}
            if (literal("null")) {return Var();}
            char *stop = nullptr;
            const std::string number(at, static_cast<size_t>(std::min<ptrdiff_t>(end - at, 64)));
            const double parsed = std::strtod(number.c_str(), &stop);
            if (stop == number.c_str()) {failed = true; return {};}
            at += stop - number.c_str();
            return Var(parsed);
        }
    };

    Type type = Null;
    double number = 0.0;
    std::string text;
    std::map<std::string, Var> properties;
    std::vector<Var> items;
};

} // namespace prism
