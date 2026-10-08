#include "cbor.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace truma_inetx {
namespace cbor {

static const int MAX_DEPTH = 16;
static const size_t MAX_ITEMS = 1024;  // per container

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

double Value::as_double() const {
  switch (this->type) {
    case Type::INTEGER:
      return static_cast<double>(this->integer);
    case Type::FLOAT:
      return this->number;
    case Type::BOOL:
      return this->boolean ? 1.0 : 0.0;
    default:
      return 0.0;
  }
}

int64_t Value::as_int() const {
  switch (this->type) {
    case Type::INTEGER:
      return this->integer;
    case Type::FLOAT:
      return static_cast<int64_t>(std::lround(this->number));
    case Type::BOOL:
      return this->boolean ? 1 : 0;
    default:
      return 0;
  }
}

const Value *Value::get(const char *key) const {
  if (this->type != Type::MAP)
    return nullptr;
  for (size_t i = 0; i + 1 < this->items.size(); i += 2) {
    const Value &k = this->items[i];
    if (k.type == Type::TEXT && k.str == key)
      return &this->items[i + 1];
  }
  return nullptr;
}

static void append_escaped(std::string &out, const std::string &s) {
  out.push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
      out += buf;
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
}

static void render(const Value &v, std::string &out, size_t max_len) {
  if (out.size() > max_len)
    return;
  char buf[48];
  switch (v.type) {
    case Type::NONE:
      out += "<invalid>";
      break;
    case Type::INTEGER:
      snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.integer));
      out += buf;
      break;
    case Type::FLOAT:
      snprintf(buf, sizeof(buf), "%g", v.number);
      out += buf;
      break;
    case Type::BOOL:
      out += v.boolean ? "true" : "false";
      break;
    case Type::NIL:
      out += "null";
      break;
    case Type::UNDEFINED:
      out += "undefined";
      break;
    case Type::TEXT:
      append_escaped(out, v.str);
      break;
    case Type::BYTES: {
      out += "h'";
      for (unsigned char c : v.str) {
        snprintf(buf, sizeof(buf), "%02x", c);
        out += buf;
        if (out.size() > max_len)
          break;
      }
      out += "'";
      break;
    }
    case Type::ARRAY:
      out.push_back('[');
      for (size_t i = 0; i < v.items.size(); i++) {
        if (i)
          out.push_back(',');
        render(v.items[i], out, max_len);
        if (out.size() > max_len)
          break;
      }
      out.push_back(']');
      break;
    case Type::MAP:
      out.push_back('{');
      for (size_t i = 0; i + 1 < v.items.size(); i += 2) {
        if (i)
          out.push_back(',');
        render(v.items[i], out, max_len);
        out.push_back(':');
        render(v.items[i + 1], out, max_len);
        if (out.size() > max_len)
          break;
      }
      out.push_back('}');
      break;
  }
}

std::string Value::to_string(size_t max_len) const {
  std::string out;
  render(*this, out, max_len);
  if (out.size() > max_len) {
    out.resize(max_len);
    out += "...";
  }
  return out;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

namespace {

// Heap accounting of a decoded tree (approximate, on the safe side).
static const size_t HEAP_BLOCK_OVERHEAD = 16;  // allocator header + alignment
static const size_t STRING_SSO_CAPACITY = 15;  // libstdc++ keeps shorter strings inside std::string

/// Used twice on the same bytes: in measuring mode (`measure` set) it walks the item without
/// allocating anything, which validates it and estimates the memory needed; then it builds the
/// tree with exact reservations, so containers never grow (no reallocation peaks).
class Decoder {
 public:
  Decoder(const uint8_t *data, size_t len, size_t pos, DecodeInfo *measure)
      : data_(data), len_(len), pos_(pos), measure_(measure) {}

  bool item(Value &out, int depth) {
    if (depth > MAX_DEPTH || this->pos_ >= this->len_)
      return false;
    uint8_t ib = this->data_[this->pos_++];
    uint8_t major = ib >> 5;
    uint8_t info = ib & 0x1F;
    if (this->measure_ != nullptr && major != 6)
      this->measure_->nodes++;

    if (major == 7)
      return this->simple_(info, out);

    uint64_t arg = 0;
    bool indefinite = false;
    if (info == 31) {
      if (major == 0 || major == 1 || major == 6)
        return false;  // indefinite length not allowed for these
      indefinite = true;
    } else if (!this->argument_(info, arg)) {
      return false;
    }

    switch (major) {
      case 0:  // unsigned integer
        if (arg > static_cast<uint64_t>(INT64_MAX))
          return false;
        out.type = Type::INTEGER;
        out.integer = static_cast<int64_t>(arg);
        return true;
      case 1:  // negative integer: -1 - arg
        if (arg > static_cast<uint64_t>(INT64_MAX))
          return false;
        out.type = Type::INTEGER;
        out.integer = -1 - static_cast<int64_t>(arg);
        return true;
      case 2:
      case 3: {
        out.type = major == 2 ? Type::BYTES : Type::TEXT;
        if (indefinite)
          return this->chunked_string_(major, out.str);
        if (arg > this->len_ - this->pos_)
          return false;
        const size_t n = static_cast<size_t>(arg);
        this->account_string_(n);
        this->read_bytes_(n, &out.str);
        return true;
      }
      case 4:
      case 5: {
        const size_t stride = major == 4 ? 1 : 2;
        out.type = major == 4 ? Type::ARRAY : Type::MAP;
        if (indefinite)
          return this->indefinite_container_(out, depth, stride);
        // every item takes at least one byte: reject impossible counts before allocating
        if (arg > MAX_ITEMS || arg * stride > this->len_ - this->pos_)
          return false;
        return this->definite_container_(out, depth, static_cast<size_t>(arg) * stride);
      }
      case 6:  // tag: decode and keep the tagged item only
        return this->item(out, depth + 1);
      default:
        return false;
    }
  }

  size_t pos() const { return this->pos_; }

 protected:
  void account_block_(size_t bytes) {
    if (this->measure_ == nullptr || bytes == 0)
      return;
    this->measure_->heap += bytes + HEAP_BLOCK_OVERHEAD;
    if (bytes > this->measure_->largest_block)
      this->measure_->largest_block = bytes;
  }
  void account_string_(size_t length) {
    if (length > STRING_SSO_CAPACITY)
      this->account_block_(length + 1);
  }

  bool argument_(uint8_t info, uint64_t &arg) {
    if (info < 24) {
      arg = info;
      return true;
    }
    size_t n;
    switch (info) {
      case 24:
        n = 1;
        break;
      case 25:
        n = 2;
        break;
      case 26:
        n = 4;
        break;
      case 27:
        n = 8;
        break;
      default:
        return false;
    }
    if (n > this->len_ - this->pos_)
      return false;
    arg = 0;
    for (size_t i = 0; i < n; i++)
      arg = (arg << 8) | this->data_[this->pos_++];
    return true;
  }

  /// Consumes `n` bytes (checked by the caller); appends them to `out` unless measuring.
  void read_bytes_(size_t n, std::string *out) {
    if (this->measure_ == nullptr && out != nullptr)
      out->append(reinterpret_cast<const char *>(this->data_ + this->pos_), n);
    this->pos_ += n;
  }

  /// Chunks of an indefinite-length string, up to its break. `total` receives the length.
  bool chunks_(uint8_t major, std::string *out, size_t *total) {
    *total = 0;
    while (true) {
      if (this->pos_ >= this->len_)
        return false;
      uint8_t ib = this->data_[this->pos_++];
      if (ib == 0xFF)
        return true;
      if ((ib >> 5) != major || (ib & 0x1F) == 31)
        return false;
      uint64_t n;
      if (!this->argument_(ib & 0x1F, n) || n > this->len_ - this->pos_)
        return false;
      *total += static_cast<size_t>(n);
      this->read_bytes_(static_cast<size_t>(n), out);
    }
  }

  bool chunked_string_(uint8_t major, std::string &out) {
    size_t total = 0;
    if (this->measure_ != nullptr) {
      if (!this->chunks_(major, nullptr, &total))
        return false;
      this->account_string_(total);
      return true;
    }
    DecodeInfo sizes;
    Decoder scan(this->data_, this->len_, this->pos_, &sizes);
    if (!scan.chunks_(major, nullptr, &total))
      return false;
    out.reserve(total);
    return this->chunks_(major, &out, &total);
  }

  bool definite_container_(Value &out, int depth, size_t count) {
    if (this->measure_ != nullptr) {
      this->account_block_(count * sizeof(Value));
      Value scratch;  // stays empty: nothing is stored while measuring
      for (size_t i = 0; i < count; i++) {
        if (!this->item(scratch, depth + 1))
          return false;
      }
      return true;
    }
    out.items.resize(count);
    for (auto &child : out.items) {
      if (!this->item(child, depth + 1))
        return false;
    }
    return true;
  }

  /// Walks the items of an indefinite container up to (not including) its break.
  bool count_items_(int depth, size_t stride, size_t *count) {
    Value scratch;
    *count = 0;
    while (true) {
      if (this->pos_ >= this->len_)
        return false;
      if (this->data_[this->pos_] == 0xFF)
        return (*count % stride) == 0;
      if (*count >= MAX_ITEMS * stride || !this->item(scratch, depth + 1))
        return false;
      (*count)++;
    }
  }

  bool indefinite_container_(Value &out, int depth, size_t stride) {
    size_t count = 0;
    if (this->measure_ != nullptr) {
      if (!this->count_items_(depth, stride, &count))
        return false;
      this->pos_++;  // break
      this->account_block_(count * sizeof(Value));
      return true;
    }
    {
      DecodeInfo sizes;
      Decoder scan(this->data_, this->len_, this->pos_, &sizes);
      if (!scan.count_items_(depth, stride, &count))
        return false;
    }
    out.items.reserve(count);
    for (size_t i = 0; i < count; i++) {
      out.items.emplace_back();
      if (!this->item(out.items.back(), depth + 1))
        return false;
    }
    if (this->pos_ >= this->len_ || this->data_[this->pos_] != 0xFF)
      return false;
    this->pos_++;
    return true;
  }

  static double half_to_double(uint16_t h) {
    int exp = (h >> 10) & 0x1F;
    int mant = h & 0x3FF;
    double val;
    if (exp == 0) {
      val = std::ldexp(mant, -24);
    } else if (exp != 31) {
      val = std::ldexp(mant + 1024, exp - 25);
    } else {
      val = mant == 0 ? INFINITY : NAN;
    }
    return (h & 0x8000) ? -val : val;
  }

  bool simple_(uint8_t info, Value &out) {
    switch (info) {
      case 20:
      case 21:
        out.type = Type::BOOL;
        out.boolean = info == 21;
        return true;
      case 22:
        out.type = Type::NIL;
        return true;
      case 23:
        out.type = Type::UNDEFINED;
        return true;
      case 24:  // simple value in next byte: treat as undefined
        if (this->pos_ >= this->len_)
          return false;
        this->pos_++;
        out.type = Type::UNDEFINED;
        return true;
      case 25: {
        uint64_t raw;
        if (!this->argument_(25, raw))
          return false;
        out.type = Type::FLOAT;
        out.number = half_to_double(static_cast<uint16_t>(raw));
        return true;
      }
      case 26: {
        uint64_t raw;
        if (!this->argument_(26, raw))
          return false;
        uint32_t bits = static_cast<uint32_t>(raw);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        out.type = Type::FLOAT;
        out.number = f;
        return true;
      }
      case 27: {
        uint64_t raw;
        if (!this->argument_(27, raw))
          return false;
        double d;
        std::memcpy(&d, &raw, sizeof(d));
        out.type = Type::FLOAT;
        out.number = d;
        return true;
      }
      default:
        if (info < 20) {  // unassigned simple values
          out.type = Type::UNDEFINED;
          return true;
        }
        return false;  // 28..31 (break outside container, reserved)
    }
  }

  const uint8_t *data_;
  size_t len_;
  size_t pos_;
  DecodeInfo *measure_;
};

}  // namespace

bool measure(const uint8_t *data, size_t len, DecodeInfo *info, size_t *consumed) {
  DecodeInfo local;
  DecodeInfo *m = info != nullptr ? info : &local;
  *m = DecodeInfo();
  if (data == nullptr || len == 0)
    return false;
  Decoder dec(data, len, 0, m);
  Value scratch;
  if (!dec.item(scratch, 0))
    return false;
  if (consumed != nullptr)
    *consumed = dec.pos();
  return true;
}

bool decode(const uint8_t *data, size_t len, Value &out, size_t *consumed, const DecodeLimits *limits,
            DecodeInfo *info) {
  out = Value();
  DecodeInfo local;
  DecodeInfo *m = info != nullptr ? info : &local;
  // 1. validate and measure without allocating: malformed or truncated data costs nothing
  if (!measure(data, len, m, nullptr))
    return false;
  // 2. refuse what would not fit in memory (an allocation failure aborts the firmware)
  if (limits != nullptr && (m->heap > limits->max_heap || m->largest_block > limits->max_block)) {
    m->too_large = true;
    return false;
  }
  // 3. build the tree
  Decoder dec(data, len, 0, nullptr);
  if (!dec.item(out, 0)) {
    out = Value();
    return false;
  }
  if (consumed != nullptr)
    *consumed = dec.pos();
  return true;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

void Encoder::head_(uint8_t major, uint64_t arg) {
  uint8_t mt = static_cast<uint8_t>(major << 5);
  if (arg < 24) {
    this->out_.push_back(mt | static_cast<uint8_t>(arg));
  } else if (arg <= 0xFF) {
    this->out_.push_back(mt | 24);
    this->out_.push_back(static_cast<uint8_t>(arg));
  } else if (arg <= 0xFFFF) {
    this->out_.push_back(mt | 25);
    this->out_.push_back(static_cast<uint8_t>(arg >> 8));
    this->out_.push_back(static_cast<uint8_t>(arg));
  } else if (arg <= 0xFFFFFFFFULL) {
    this->out_.push_back(mt | 26);
    for (int s = 24; s >= 0; s -= 8)
      this->out_.push_back(static_cast<uint8_t>(arg >> s));
  } else {
    this->out_.push_back(mt | 27);
    for (int s = 56; s >= 0; s -= 8)
      this->out_.push_back(static_cast<uint8_t>(arg >> s));
  }
}

void Encoder::text(const char *s) { this->text_(s, std::strlen(s)); }

void Encoder::text_(const char *s, size_t len) {
  this->head_(3, len);
  this->out_.insert(this->out_.end(), s, s + len);
}

void Encoder::bytes(const uint8_t *data, size_t len) {
  this->head_(2, len);
  this->out_.insert(this->out_.end(), data, data + len);
}

void Encoder::integer(int64_t v) {
  if (v >= 0) {
    this->head_(0, static_cast<uint64_t>(v));
  } else {
    this->head_(1, static_cast<uint64_t>(-1 - v));
  }
}

void Encoder::float64(double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  this->out_.push_back(0xFB);
  for (int s = 56; s >= 0; s -= 8)
    this->out_.push_back(static_cast<uint8_t>(bits >> s));
}

void Encoder::value(const Value &v) {
  switch (v.type) {
    case Type::INTEGER:
      this->integer(v.integer);
      break;
    case Type::FLOAT:
      this->float64(v.number);
      break;
    case Type::BOOL:
      this->boolean(v.boolean);
      break;
    case Type::TEXT:
      this->text(v.str);
      break;
    case Type::BYTES:
      this->bytes(reinterpret_cast<const uint8_t *>(v.str.data()), v.str.size());
      break;
    case Type::ARRAY:
      this->array(v.items.size());
      for (const auto &item : v.items)
        this->value(item);
      break;
    case Type::MAP:
      this->map(v.items.size() / 2);
      for (const auto &item : v.items)
        this->value(item);
      break;
    case Type::UNDEFINED:
      this->out_.push_back(0xF7);
      break;
    case Type::NIL:
    case Type::NONE:
    default:
      this->null();
      break;
  }
}

}  // namespace cbor
}  // namespace truma_inetx
}  // namespace esphome
