// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file parser.cpp
 * @brief NxJSON parser implementation
 * 
 * Fast, minimal JSON parser with zero-copy string handling.
 */

#include "nxjson.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>

// ============================================================================
// Internal Value Structure
// ============================================================================

struct NxJsonValue {
    NxJsonType type;
    
    union {
        bool bool_val;
        double num_val;
        struct {
            char* str;
            size_t len;
        } str_val;
        std::vector<NxJsonValue*>* array_val;
        std::vector<std::pair<std::string, NxJsonValue*>>* object_val;
    };
    
    NxJsonValue() : type(NX_JSON_NULL) {}
    ~NxJsonValue();
};

NxJsonValue::~NxJsonValue() {
    switch (type) {
        case NX_JSON_STRING:
            free(str_val.str);
            break;
        case NX_JSON_ARRAY:
            if (array_val) {
                for (auto* v : *array_val) delete v;
                delete array_val;
            }
            break;
        case NX_JSON_OBJECT:
            if (object_val) {
                for (auto& p : *object_val) delete p.second;
                delete object_val;
            }
            break;
        default:
            break;
    }
}

// ============================================================================
// Parser State
// ============================================================================

struct Parser {
    const char* json;
    const char* end;
    const char* pos;
    int line;
    int column;
    NxJsonParseError* error;
    int depth;
    static const int MAX_DEPTH = 512;
    
    Parser(const char* j, size_t len, NxJsonParseError* err)
        : json(j), end(j + len), pos(j), line(1), column(1), error(err), depth(0) {}
    
    char peek() const { return pos < end ? *pos : '\0'; }
    char advance() {
        if (pos >= end) return '\0';
        char c = *pos++;
        if (c == '\n') { line++; column = 1; }
        else { column++; }
        return c;
    }
    
    void skipWhitespace() {
        while (pos < end) {
            char c = *pos;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else {
                break;
            }
        }
    }
    
    bool match(char expected) {
        skipWhitespace();
        if (peek() == expected) {
            advance();
            return true;
        }
        return false;
    }
    
    void setError(NxJsonError code, const char* msg) {
        if (error) {
            error->code = code;
            error->line = line;
            error->column = column;
            error->message = msg;
        }
    }
    
    NxJsonValue* parseValue();
    NxJsonValue* parseNull();
    NxJsonValue* parseBool();
    NxJsonValue* parseNumber();
    NxJsonValue* parseString();
    NxJsonValue* parseArray();
    NxJsonValue* parseObject();
    std::string parseStringContent();
};

// ============================================================================
// Parser Implementation
// ============================================================================

NxJsonValue* Parser::parseValue() {
    skipWhitespace();
    
    if (depth++ > MAX_DEPTH) {
        setError(NX_JSON_ERROR_DEPTH_LIMIT, "Maximum nesting depth exceeded");
        depth--;
        return nullptr;
    }
    
    NxJsonValue* result = nullptr;
    char c = peek();
    
    switch (c) {
        case 'n': result = parseNull(); break;
        case 't': case 'f': result = parseBool(); break;
        case '"': result = parseString(); break;
        case '[': result = parseArray(); break;
        case '{': result = parseObject(); break;
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            result = parseNumber();
            break;
        case '\0':
            setError(NX_JSON_ERROR_UNEXPECTED_EOF, "Unexpected end of input");
            break;
        default:
            setError(NX_JSON_ERROR_UNEXPECTED_TOKEN, "Unexpected character");
            break;
    }
    
    depth--;
    return result;
}

NxJsonValue* Parser::parseNull() {
    if (pos + 4 <= end && memcmp(pos, "null", 4) == 0) {
        pos += 4; column += 4;
        auto* v = new NxJsonValue();
        v->type = NX_JSON_NULL;
        return v;
    }
    setError(NX_JSON_ERROR_SYNTAX, "Expected 'null'");
    return nullptr;
}

NxJsonValue* Parser::parseBool() {
    if (pos + 4 <= end && memcmp(pos, "true", 4) == 0) {
        pos += 4; column += 4;
        auto* v = new NxJsonValue();
        v->type = NX_JSON_BOOL;
        v->bool_val = true;
        return v;
    }
    if (pos + 5 <= end && memcmp(pos, "false", 5) == 0) {
        pos += 5; column += 5;
        auto* v = new NxJsonValue();
        v->type = NX_JSON_BOOL;
        v->bool_val = false;
        return v;
    }
    setError(NX_JSON_ERROR_SYNTAX, "Expected 'true' or 'false'");
    return nullptr;
}

NxJsonValue* Parser::parseNumber() {
    const char* start = pos;
    
    // Optional minus
    if (peek() == '-') advance();
    
    // Integer part
    if (peek() == '0') {
        advance();
    } else if (peek() >= '1' && peek() <= '9') {
        while (peek() >= '0' && peek() <= '9') advance();
    } else {
        setError(NX_JSON_ERROR_INVALID_NUMBER, "Invalid number");
        return nullptr;
    }
    
    // Fraction
    if (peek() == '.') {
        advance();
        if (peek() < '0' || peek() > '9') {
            setError(NX_JSON_ERROR_INVALID_NUMBER, "Expected digit after decimal point");
            return nullptr;
        }
        while (peek() >= '0' && peek() <= '9') advance();
    }
    
    // Exponent
    if (peek() == 'e' || peek() == 'E') {
        advance();
        if (peek() == '+' || peek() == '-') advance();
        if (peek() < '0' || peek() > '9') {
            setError(NX_JSON_ERROR_INVALID_NUMBER, "Expected digit in exponent");
            return nullptr;
        }
        while (peek() >= '0' && peek() <= '9') advance();
    }
    
    std::string numStr(start, pos);
    double num = std::strtod(numStr.c_str(), nullptr);
    
    auto* v = new NxJsonValue();
    v->type = NX_JSON_NUMBER;
    v->num_val = num;
    return v;
}

std::string Parser::parseStringContent() {
    std::string result;
    
    while (peek() != '"' && peek() != '\0') {
        char c = advance();
        
        if (c == '\\') {
            c = advance();
            switch (c) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': {
                    // Parse \uXXXX
                    if (pos + 4 > end) {
                        setError(NX_JSON_ERROR_INVALID_ESCAPE, "Incomplete unicode escape");
                        return "";
                    }
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        c = advance();
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= c - '0';
                        else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                        else {
                            setError(NX_JSON_ERROR_INVALID_ESCAPE, "Invalid unicode escape");
                            return "";
                        }
                    }
                    // Encode as UTF-8
                    if (cp < 0x80) {
                        result += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        result += static_cast<char>(0xC0 | (cp >> 6));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        result += static_cast<char>(0xE0 | (cp >> 12));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    setError(NX_JSON_ERROR_INVALID_ESCAPE, "Invalid escape sequence");
                    return "";
            }
        } else {
            result += c;
        }
    }
    
    return result;
}

NxJsonValue* Parser::parseString() {
    if (!match('"')) {
        setError(NX_JSON_ERROR_SYNTAX, "Expected '\"'");
        return nullptr;
    }
    
    std::string content = parseStringContent();
    if (error && error->code != NX_JSON_OK) return nullptr;
    
    if (!match('"')) {
        setError(NX_JSON_ERROR_SYNTAX, "Unterminated string");
        return nullptr;
    }
    
    auto* v = new NxJsonValue();
    v->type = NX_JSON_STRING;
    v->str_val.len = content.size();
    v->str_val.str = static_cast<char*>(malloc(content.size() + 1));
    memcpy(v->str_val.str, content.c_str(), content.size() + 1);
    return v;
}

NxJsonValue* Parser::parseArray() {
    if (!match('[')) {
        setError(NX_JSON_ERROR_SYNTAX, "Expected '['");
        return nullptr;
    }
    
    auto* v = new NxJsonValue();
    v->type = NX_JSON_ARRAY;
    v->array_val = new std::vector<NxJsonValue*>();
    
    skipWhitespace();
    if (peek() == ']') {
        advance();
        return v;
    }
    
    while (true) {
        NxJsonValue* elem = parseValue();
        if (!elem) {
            delete v;
            return nullptr;
        }
        v->array_val->push_back(elem);
        
        skipWhitespace();
        if (match(']')) break;
        if (!match(',')) {
            setError(NX_JSON_ERROR_SYNTAX, "Expected ',' or ']'");
            delete v;
            return nullptr;
        }
    }
    
    return v;
}

NxJsonValue* Parser::parseObject() {
    if (!match('{')) {
        setError(NX_JSON_ERROR_SYNTAX, "Expected '{'");
        return nullptr;
    }
    
    auto* v = new NxJsonValue();
    v->type = NX_JSON_OBJECT;
    v->object_val = new std::vector<std::pair<std::string, NxJsonValue*>>();
    
    skipWhitespace();
    if (peek() == '}') {
        advance();
        return v;
    }
    
    while (true) {
        skipWhitespace();
        if (peek() != '"') {
            setError(NX_JSON_ERROR_SYNTAX, "Expected string key");
            delete v;
            return nullptr;
        }
        
        advance(); // Skip opening quote
        std::string key = parseStringContent();
        if (error && error->code != NX_JSON_OK) {
            delete v;
            return nullptr;
        }
        if (!match('"')) {
            setError(NX_JSON_ERROR_SYNTAX, "Unterminated key string");
            delete v;
            return nullptr;
        }
        
        if (!match(':')) {
            setError(NX_JSON_ERROR_SYNTAX, "Expected ':'");
            delete v;
            return nullptr;
        }
        
        NxJsonValue* val = parseValue();
        if (!val) {
            delete v;
            return nullptr;
        }
        
        v->object_val->emplace_back(std::move(key), val);
        
        skipWhitespace();
        if (match('}')) break;
        if (!match(',')) {
            setError(NX_JSON_ERROR_SYNTAX, "Expected ',' or '}'");
            delete v;
            return nullptr;
        }
    }
    
    return v;
}

// ============================================================================
// C API Implementation
// ============================================================================

extern "C" {

NxJsonValue* nx_json_parse(const char* json, NxJsonParseError* error) {
    if (!json) return nullptr;
    return nx_json_parse_len(json, strlen(json), error);
}

NxJsonValue* nx_json_parse_len(const char* json, size_t len, NxJsonParseError* error) {
    if (!json) return nullptr;
    
    if (error) {
        error->code = NX_JSON_OK;
        error->line = 0;
        error->column = 0;
        error->message = nullptr;
    }
    
    Parser parser(json, len, error);
    NxJsonValue* result = parser.parseValue();
    
    if (result) {
        parser.skipWhitespace();
        if (parser.peek() != '\0') {
            parser.setError(NX_JSON_ERROR_SYNTAX, "Unexpected data after JSON value");
            delete result;
            return nullptr;
        }
    }
    
    return result;
}

void nx_json_free(NxJsonValue* value) {
    delete value;
}

NxJsonType nx_json_type(const NxJsonValue* value) {
    return value ? value->type : NX_JSON_NULL;
}

bool nx_json_is_null(const NxJsonValue* value) { return !value || value->type == NX_JSON_NULL; }
bool nx_json_is_bool(const NxJsonValue* value) { return value && value->type == NX_JSON_BOOL; }
bool nx_json_is_number(const NxJsonValue* value) { return value && value->type == NX_JSON_NUMBER; }
bool nx_json_is_string(const NxJsonValue* value) { return value && value->type == NX_JSON_STRING; }
bool nx_json_is_array(const NxJsonValue* value) { return value && value->type == NX_JSON_ARRAY; }
bool nx_json_is_object(const NxJsonValue* value) { return value && value->type == NX_JSON_OBJECT; }

bool nx_json_get_bool(const NxJsonValue* value) {
    return value && value->type == NX_JSON_BOOL ? value->bool_val : false;
}

double nx_json_get_number(const NxJsonValue* value) {
    return value && value->type == NX_JSON_NUMBER ? value->num_val : 0.0;
}

int64_t nx_json_get_int(const NxJsonValue* value) {
    return value && value->type == NX_JSON_NUMBER ? static_cast<int64_t>(value->num_val) : 0;
}

const char* nx_json_get_string(const NxJsonValue* value) {
    return value && value->type == NX_JSON_STRING ? value->str_val.str : "";
}

size_t nx_json_get_string_len(const NxJsonValue* value) {
    return value && value->type == NX_JSON_STRING ? value->str_val.len : 0;
}

size_t nx_json_array_size(const NxJsonValue* value) {
    return value && value->type == NX_JSON_ARRAY ? value->array_val->size() : 0;
}

NxJsonValue* nx_json_array_get(const NxJsonValue* value, size_t index) {
    if (!value || value->type != NX_JSON_ARRAY) return nullptr;
    if (index >= value->array_val->size()) return nullptr;
    return (*value->array_val)[index];
}

size_t nx_json_object_size(const NxJsonValue* value) {
    return value && value->type == NX_JSON_OBJECT ? value->object_val->size() : 0;
}

NxJsonValue* nx_json_object_get(const NxJsonValue* value, const char* key) {
    if (!value || value->type != NX_JSON_OBJECT || !key) return nullptr;
    for (const auto& p : *value->object_val) {
        if (p.first == key) return p.second;
    }
    return nullptr;
}

bool nx_json_object_has(const NxJsonValue* value, const char* key) {
    return nx_json_object_get(value, key) != nullptr;
}

size_t nx_json_object_count(const NxJsonValue* value) {
    return nx_json_object_size(value);
}

NxJsonMember nx_json_object_at(const NxJsonValue* value, size_t index) {
    NxJsonMember m = {nullptr, nullptr};
    if (!value || value->type != NX_JSON_OBJECT) return m;
    if (index >= value->object_val->size()) return m;
    const auto& p = (*value->object_val)[index];
    m.key = p.first.c_str();
    m.value = p.second;
    return m;
}

// Value creation
NxJsonValue* nx_json_null() {
    return new NxJsonValue();
}

NxJsonValue* nx_json_bool(bool value) {
    auto* v = new NxJsonValue();
    v->type = NX_JSON_BOOL;
    v->bool_val = value;
    return v;
}

NxJsonValue* nx_json_number(double value) {
    auto* v = new NxJsonValue();
    v->type = NX_JSON_NUMBER;
    v->num_val = value;
    return v;
}

NxJsonValue* nx_json_int(int64_t value) {
    return nx_json_number(static_cast<double>(value));
}

NxJsonValue* nx_json_string(const char* value) {
    return nx_json_string_len(value, value ? strlen(value) : 0);
}

NxJsonValue* nx_json_string_len(const char* value, size_t len) {
    auto* v = new NxJsonValue();
    v->type = NX_JSON_STRING;
    v->str_val.len = len;
    v->str_val.str = static_cast<char*>(malloc(len + 1));
    if (value && len > 0) memcpy(v->str_val.str, value, len);
    v->str_val.str[len] = '\0';
    return v;
}

NxJsonValue* nx_json_array() {
    auto* v = new NxJsonValue();
    v->type = NX_JSON_ARRAY;
    v->array_val = new std::vector<NxJsonValue*>();
    return v;
}

NxJsonValue* nx_json_object() {
    auto* v = new NxJsonValue();
    v->type = NX_JSON_OBJECT;
    v->object_val = new std::vector<std::pair<std::string, NxJsonValue*>>();
    return v;
}

void nx_json_array_push(NxJsonValue* array, NxJsonValue* value) {
    if (array && array->type == NX_JSON_ARRAY && value) {
        array->array_val->push_back(value);
    }
}

void nx_json_object_set(NxJsonValue* object, const char* key, NxJsonValue* value) {
    if (!object || object->type != NX_JSON_OBJECT || !key || !value) return;
    
    // Check for existing key
    for (auto& p : *object->object_val) {
        if (p.first == key) {
            delete p.second;
            p.second = value;
            return;
        }
    }
    object->object_val->emplace_back(key, value);
}

} // extern "C"
