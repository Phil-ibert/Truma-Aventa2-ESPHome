#include "frame.h"

#include <cstdio>

namespace esphome {
namespace truma_inetx {

static inline uint16_t read_u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t read_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
static inline void push_u16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

size_t expected_frame_length(const uint8_t *data, size_t len) {
  if (data == nullptr || len < 6)
    return 0;
  return 7 + static_cast<size_t>(read_u16(data + 4));
}

bool parse_frame(const uint8_t *data, size_t len, Frame &out, const cbor::DecodeLimits *limits) {
  out = Frame();
  if (data == nullptr || len < V3_HEADER_SIZE)
    return false;
  out.dest = read_u16(data + 0);
  out.src = read_u16(data + 2);
  out.packet_size = read_u16(data + 4);
  out.control = data[6];
  out.seg_flags = data[7];
  out.seg_number = read_u16(data + 8);
  out.seg_count = read_u16(data + 10);
  out.seg_size = read_u32(data + 12);
  if (len > V3_HEADER_SIZE)
    out.sub_type = data[16];
  if (len > V3_HEADER_SIZE + 1)
    out.corr_id = data[17];
  if (len > V3_MIN_FRAME) {
    out.payload.assign(data + V3_MIN_FRAME, data + len);
    size_t consumed = 0;
    if (!cbor::decode(out.payload.data(), out.payload.size(), out.cbor, &consumed, limits, &out.cbor_info))
      out.cbor = cbor::Value();
  }
  return true;
}

std::vector<uint8_t> build_frame(uint16_t dest, uint16_t src, uint8_t control, uint8_t sub_type, uint8_t corr_id,
                                 const std::vector<uint8_t> &cbor_payload) {
  std::vector<uint8_t> out;
  out.reserve(V3_MIN_FRAME + cbor_payload.size());
  push_u16(out, dest);
  push_u16(out, src);
  // packet size counts the 9-byte segmentation header + sub-protocol header + payload
  push_u16(out, static_cast<uint16_t>(2 + cbor_payload.size() + 9));
  out.push_back(control);
  out.insert(out.end(), 9, 0x00);  // not segmented
  out.push_back(sub_type);
  out.push_back(corr_id);
  out.insert(out.end(), cbor_payload.begin(), cbor_payload.end());
  return out;
}

std::vector<uint8_t> build_registration(uint16_t src) {
  std::vector<uint8_t> payload;
  cbor::Encoder enc(payload);
  enc.map(1);
  enc.text("pv");
  enc.array(2);
  enc.integer(5);
  enc.integer(1);
  return build_frame(ADDR_BROADCAST, src, CTRL_REGISTRATION, REG_REQUEST, 0x42, payload);
}

std::vector<uint8_t> build_subscribe(uint16_t src, const std::vector<std::string> &topics) {
  std::vector<uint8_t> payload;
  cbor::Encoder enc(payload);
  enc.map(1);
  enc.text("tn");
  enc.array(topics.size());
  for (const auto &t : topics)
    enc.text(t);
  return build_frame(ADDR_MESSAGE_BROKER, src, CTRL_MBP, MBP_SUBSCRIBE, 0, payload);
}

std::vector<uint8_t> build_system_time_topics(uint16_t src, uint16_t dest, int64_t time, int64_t lot) {
  std::vector<uint8_t> payload;
  cbor::Encoder enc(payload);
  enc.map(2);
  enc.text("avail");
  enc.integer(1);
  enc.text("topics");
  enc.array(1);
  enc.map(3);
  enc.text("tn");
  enc.text("SystemTime");
  enc.text("id");
  enc.integer(0);
  enc.text("parameters");
  enc.array(2);
  const struct {
    const char *name;
    int64_t value;
    int64_t type;
  } params[2] = {{"Time", time, 18}, {"Lot", lot, 1}};
  for (const auto &p : params) {
    enc.map(5);
    enc.text("v");
    enc.integer(p.value);
    enc.text("id");
    enc.integer(0);
    enc.text("type");
    enc.integer(p.type);
    enc.text("pn");
    enc.text(p.name);
    enc.text("tn");
    enc.text("SystemTime");
  }
  return build_frame(dest, src, CTRL_MBP, MBP_WRITE, 0, payload);
}

std::vector<uint8_t> build_write(uint16_t src, uint16_t dest, const std::string &topic, const std::string &param,
                                 const cbor::Value &value) {
  std::vector<uint8_t> payload;
  cbor::Encoder enc(payload);
  enc.map(4);
  enc.text("tn");
  enc.text(topic);
  enc.text("pn");
  enc.text(param);
  enc.text("v");
  enc.value(value);
  enc.text("id");
  enc.integer(0);
  return build_frame(dest, src, CTRL_MBP, MBP_WRITE, 0, payload);
}

std::vector<uint8_t> build_write_map(uint16_t src, uint16_t dest, const char *key, int64_t value) {
  std::vector<uint8_t> payload;
  cbor::Encoder enc(payload);
  enc.map(1);
  enc.text(key);
  enc.integer(value);
  return build_frame(dest, src, CTRL_MBP, MBP_WRITE, 0, payload);
}

std::vector<uint8_t> build_param_discovery(uint16_t src, uint16_t dest) {
  return build_frame(dest, src, CTRL_MBP, MBP_PARAM_DISCOVERY, 0, {});
}

std::string hex_dump(const uint8_t *data, size_t len, size_t max_bytes) {
  std::string out;
  size_t n = len < max_bytes ? len : max_bytes;
  out.reserve(n * 3 + 8);
  char buf[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%02x", data[i]);
    if (i)
      out.push_back(' ');
    out += buf;
  }
  if (len > n)
    out += " ...";
  return out;
}

const char *control_name(uint8_t control) {
  switch (control) {
    case CTRL_REGISTRATION:
      return "REGISTRATION";
    case CTRL_DISCOVERY:
      return "DISCOVERY";
    case CTRL_MBP:
      return "MBP";
    case CTRL_FILE_MANAGER:
      return "FILE_MANAGER";
    case CTRL_SECURITY:
      return "SECURITY";
    case CTRL_FIRMWARE:
      return "FIRMWARE";
    default:
      return "UNKNOWN";
  }
}

const char *mbp_name(uint8_t sub_type) {
  switch (sub_type) {
    case MBP_INFO:
      return "INFO";
    case MBP_WRITE:
      return "WRITE";
    case MBP_SUBSCRIBE:
      return "SUBSCRIBE";
    case MBP_BINARY:
      return "BINARY";
    case MBP_PARAM_DISCOVERY:
      return "PARAM_DISCOVERY";
    case MBP_SUBSCRIBE_RESPONSE:
      return "SUBSCRIBE_RESPONSE";
    case MBP_PARAM_DISCOVERY_RESPONSE:
      return "PARAM_DISCOVERY_RESPONSE";
    default:
      return "UNKNOWN";
  }
}

}  // namespace truma_inetx
}  // namespace esphome
