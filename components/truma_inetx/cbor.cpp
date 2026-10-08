#include "cbor.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace truma_inetx {
namespace cbor {

static const int MAX_DEPTH = 16;
static const size_t MAX_ITEMS = 4096;

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

class Decoder {
 public:
  Decoder(const uint8_t *data, size_t len) : data_(data), len_(len) {}

  bool item(Value &out, int depth) {
    if (depth > MAX_DEPTH || this->pos_ >= this->len_)
      return false;
    uint8_t ib = this->data_[this->pos_++];
    uint8_t major = ib >> 5;
    uint8_t info = ib & 0x1F;

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
      case 3:
        out.type = major == 2 ? Type::BYTES : Type::TEXT;
        if (indefinite)
          return this->chunked_string_(major, out.str);
        return this->read_bytes_(arg, out.str);
      case 4: {
        out.type = Type::ARRAY;
        if (indefinite)
          return this->indefinite_container_(out, depth, 1);
        if (arg > MAX_ITEMS)
          return false;
        out.items.resize(static_cast<size_t>(arg));
        for (auto &child : out.items) {
          if (!this->item(child, depth + 1))
            return false;
        }
        return true;
      }
      case 5: {
        out.type = Type::MAP;
        if (indefinite)
          return this->indefinite_container_(out, depth, 2);
        if (arg > MAX_ITEMS)
          return false;
        out.items.resize(static_cast<size_t>(arg) * 2);
        for (auto &child : out.items) {
          if (!this->item(child, depth + 1))
            return false;
        }
        return true;
      }
      case 6:  // tag: decode and keep the tagged item only
        return this->item(out, depth + 1);
      default:
        return false;
    }
  }

  size_t pos() const { return this->pos_; }

 protected:
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
    if (this->pos_ + n > this->len_)
      return false;
    arg = 0;
    for (size_t i = 0; i < n; i++)
      arg = (arg << 8) | this->data_[this->pos_++];
    return true;
  }

  bool read_bytes_(uint64_t n, std::string &out) {
    if (n > this->len_ - this->pos_)
      return false;
    out.append(reinterpret_cast<const char *>(this->data_ + this->pos_), static_cast<size_t>(n));
    this->pos_ += static_cast<size_t>(n);
    return true;
  }

  bool chunked_string_(uint8_t major, std::string &out) {
    while (true) {
      if (this->pos_ >= this->len_)
        return false;
      uint8_t ib = this->data_[this->pos_];
      if (ib == 0xFF) {
        this->pos_++;
        return true;
      }
      this->pos_++;
      if ((ib >> 5) != major)
        return false;
      uint64_t n;
      if ((ib & 0x1F) == 31 || !this->argument_(ib & 0x1F, n))
        return false;
      if (!this->read_bytes_(n, out))
        return false;
    }
  }

  bool indefinite_container_(Value &out, int depth, size_t stride) {
    while (true) {
      if (this->pos_ >= this->len_)
        return false;
      if (this->data_[this->pos_] == 0xFF) {
        this->pos_++;
        return (out.items.size() % stride) == 0;
      }
      if (out.items.size() >= MAX_ITEMS * stride)
        return false;
      out.items.emplace_back();
      if (!this->item(out.items.back(), depth + 1))
        return false;
    }
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
  size_t pos_{0};
};

}  // namespace

bool decode(const uint8_t *data, size_t len, Value &out, size_t *consumed) {
  out = Value();
  if (data == nullptr || len == 0)
    return false;
  Decoder dec(data, len);
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
