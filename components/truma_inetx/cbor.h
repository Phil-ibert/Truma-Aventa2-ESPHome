#pragma once

// Minimal CBOR (RFC 8949) codec used by the Truma iNet X protocol.
// Pure C++17, no ESPHome/ESP-IDF dependency, so it can be unit-tested on a PC.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace truma_inetx {
namespace cbor {

enum class Type : uint8_t {
  NONE = 0,   // decode error / not set
  INTEGER,    // major types 0 and 1
  BYTES,      // major type 2
  TEXT,       // major type 3
  ARRAY,      // major type 4
  MAP,        // major type 5 (items = key, value, key, value, ...)
  BOOL,       // simple 20/21
  NIL,        // simple 22
  UNDEFINED,  // simple 23
  FLOAT,      // half / single / double
};

struct Value {
  Type type{Type::NONE};
  int64_t integer{0};
  double number{0.0};
  bool boolean{false};
  std::string str;            // TEXT or BYTES
  std::vector<Value> items;   // ARRAY items, or MAP as alternating key/value

  bool valid() const { return this->type != Type::NONE; }
  bool is_integer() const { return this->type == Type::INTEGER; }
  bool is_number() const { return this->type == Type::INTEGER || this->type == Type::FLOAT || this->type == Type::BOOL; }
  bool is_text() const { return this->type == Type::TEXT; }
  bool is_map() const { return this->type == Type::MAP; }
  bool is_array() const { return this->type == Type::ARRAY; }

  /// Numeric view (integer, float or bool). Returns 0 for other types.
  double as_double() const;
  /// Integer view (floats are rounded). Returns 0 for non-numeric types.
  int64_t as_int() const;

  /// Map lookup by text key. Returns nullptr if not a map or key missing.
  const Value *get(const char *key) const;
  size_t map_size() const { return this->type == Type::MAP ? this->items.size() / 2 : 0; }
  const Value &map_key(size_t i) const { return this->items[i * 2]; }
  const Value &map_value(size_t i) const { return this->items[i * 2 + 1]; }

  /// Compact JSON-like rendering, for logs.
  std::string to_string(size_t max_len = 512) const;

  static Value make_int(int64_t v) {
    Value r;
    r.type = Type::INTEGER;
    r.integer = v;
    return r;
  }
  static Value make_text(const std::string &v) {
    Value r;
    r.type = Type::TEXT;
    r.str = v;
    return r;
  }
};

/// Decode one CBOR item. Returns true on success; `consumed` receives the number of bytes used.
bool decode(const uint8_t *data, size_t len, Value &out, size_t *consumed = nullptr);

/// Streaming encoder appending to a byte vector (definite lengths only, like the Truma app).
class Encoder {
 public:
  explicit Encoder(std::vector<uint8_t> &out) : out_(out) {}
  void map(size_t pairs) { this->head_(5, pairs); }
  void array(size_t count) { this->head_(4, count); }
  void text(const char *s);
  void text(const std::string &s) { this->text_(s.data(), s.size()); }
  void bytes(const uint8_t *data, size_t len);
  void integer(int64_t v);
  void boolean(bool v) { this->out_.push_back(v ? 0xF5 : 0xF4); }
  void null() { this->out_.push_back(0xF6); }
  void float64(double v);
  void value(const Value &v);

 protected:
  void head_(uint8_t major, uint64_t arg);
  void text_(const char *s, size_t len);
  std::vector<uint8_t> &out_;
};

}  // namespace cbor
}  // namespace truma_inetx
}  // namespace esphome
