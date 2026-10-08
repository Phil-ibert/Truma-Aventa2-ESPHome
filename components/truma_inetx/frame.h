#pragma once

// TruMessageV3 framing used by Truma iNet X over BLE.
// Pure C++17 (no ESPHome dependency) so it can be unit-tested on a PC.
//
// Layout (little endian):
//   [0-1]   destination device id
//   [2-3]   source device id
//   [4-5]   packet size = (sub-protocol header + CBOR) + 9
//   [6]     control type
//   [7]     segmentation flags
//   [8-9]   segment number
//   [10-11] segment count
//   [12-15] message size / offset
//   [16]    sub-protocol type (e.g. MBP type)
//   [17]    correlation id
//   [18..]  CBOR payload
//
// Protocol documented by https://github.com/daaaaan/truma-inetx-ble

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cbor.h"

namespace esphome {
namespace truma_inetx {

// GATT (all UUIDs share the base -F3B2-11E8-8EB2-F2801F1B9FD1)
// The characteristics live in different services depending on the device:
//   Aventa 2nd generation (observed): FC314000-...   iNet X panel: F47BBBAC-...
static const char *const SERVICE_UUID_AVENTA = "FC314000-F3B2-11E8-8EB2-F2801F1B9FD1";
static const char *const SERVICE_UUID_PANEL = "F47BBBAC-F3B2-11E8-8EB2-F2801F1B9FD1";
static const char *const SERVICE_UUID = SERVICE_UUID_PANEL;  // kept for compatibility
static const char *const CHAR_CMD_UUID = "FC314001-F3B2-11E8-8EB2-F2801F1B9FD1";     // write (with response) + notify
static const char *const CHAR_DATA_W_UUID = "FC314002-F3B2-11E8-8EB2-F2801F1B9FD1";  // write without response
static const char *const CHAR_DATA_R_UUID = "FC314003-F3B2-11E8-8EB2-F2801F1B9FD1";  // notify
// FC314004 (CMD_ALT) exists too: never subscribe to it (breaks the transport).

static const uint16_t TRUMA_MANUFACTURER_ID = 0x0C73;

// Device addresses
static const uint16_t ADDR_MESSAGE_BROKER = 0x0000;
static const uint16_t ADDR_PANEL = 0x0101;
static const uint16_t ADDR_APP_DEFAULT = 0x0500;
static const uint16_t ADDR_BROADCAST = 0xFFFF;

// Control types (byte 6)
static const uint8_t CTRL_REGISTRATION = 0x01;
static const uint8_t CTRL_DISCOVERY = 0x02;
static const uint8_t CTRL_MBP = 0x03;
static const uint8_t CTRL_FILE_MANAGER = 0x04;
static const uint8_t CTRL_SECURITY = 0x05;
static const uint8_t CTRL_FIRMWARE = 0x06;

// Registration sub types
static const uint8_t REG_REQUEST = 0x01;
static const uint8_t REG_RESPONSE = 0x02;

// Message Broker Protocol sub types (byte 16)
static const uint8_t MBP_INFO = 0x00;
static const uint8_t MBP_WRITE = 0x01;
static const uint8_t MBP_SUBSCRIBE = 0x02;
static const uint8_t MBP_BINARY = 0x03;
static const uint8_t MBP_PARAM_DISCOVERY = 0x04;
static const uint8_t MBP_SUBSCRIBE_RESPONSE = 0x82;
static const uint8_t MBP_PARAM_DISCOVERY_RESPONSE = 0x84;

// Segmentation flags (byte 7)
static const uint8_t SEG_IS_SEGMENTED = 0x01;
static const uint8_t SEG_MORE_SEGMENTS = 0x02;

// Transport opcodes on the CMD characteristic
static const uint8_t TP_INIT_DATA_TRANSFER = 0x01;  // app -> device: 01 <len lo> <len hi>
static const uint8_t TP_CONFIRM = 0x03;             // app -> device: 03 00 (after a 83 xx 00)
static const uint8_t TP_READY = 0x81;               // device -> app: 81 00
static const uint8_t TP_MSG_ACK = 0x83;             // device -> app: 83 <id> 00
static const uint8_t TP_DATA_ACK = 0xF0;            // both ways: f0 <status> (01 = transfer completed)

static const size_t V3_HEADER_SIZE = 16;
static const size_t V3_MIN_FRAME = 18;  // header + sub type + correlation id

struct Frame {
  uint16_t dest{0};
  uint16_t src{0};
  uint16_t packet_size{0};
  uint8_t control{0};
  uint8_t seg_flags{0};
  uint16_t seg_number{0};
  uint16_t seg_count{0};
  uint32_t seg_size{0};
  uint8_t sub_type{0};
  uint8_t corr_id{0};
  std::vector<uint8_t> payload;  // CBOR bytes (after sub type + correlation id)
  cbor::Value cbor;              // decoded payload (type NONE if absent / not CBOR)
  cbor::DecodeInfo cbor_info;    // size of the payload, or too_large if it was refused
};

/// Total frame length announced by a V3 header (7 + packet_size). 0 if fewer than 6 bytes are available.
size_t expected_frame_length(const uint8_t *data, size_t len);

/// Parse a V3 frame. Returns false if too short to contain a header. The CBOR payload is only
/// decoded if it is valid and fits in `limits` (see cbor::decode).
bool parse_frame(const uint8_t *data, size_t len, Frame &out, const cbor::DecodeLimits *limits = nullptr);

/// Build a non-segmented V3 frame.
std::vector<uint8_t> build_frame(uint16_t dest, uint16_t src, uint8_t control, uint8_t sub_type, uint8_t corr_id,
                                 const std::vector<uint8_t> &cbor_payload);

// --- Message builders (payloads as captured from the official app) ---------

/// Registration request: {"pv": [5, 1]} to broadcast, ctrl 0x01, sub 0x01, corr 0x42.
std::vector<uint8_t> build_registration(uint16_t src);

/// Topic subscription: {"tn": [topics...]} to the message broker.
std::vector<uint8_t> build_subscribe(uint16_t src, const std::vector<std::string> &topics);

/// Parameter write: {"tn": topic, "pn": param, "v": value, "id": 0}.
std::vector<uint8_t> build_write(uint16_t src, uint16_t dest, const std::string &topic, const std::string &param,
                                 const cbor::Value &value);

/// Bare map write such as {"LastMessage": 1}.
std::vector<uint8_t> build_write_map(uint16_t src, uint16_t dest, const char *key, int64_t value);

/// Parameter discovery request (empty payload) to a device.
std::vector<uint8_t> build_param_discovery(uint16_t src, uint16_t dest);

/// Hex dump helper for logs ("01 02 ab ...", truncated).
std::string hex_dump(const uint8_t *data, size_t len, size_t max_bytes = 64);

const char *control_name(uint8_t control);
const char *mbp_name(uint8_t sub_type);

}  // namespace truma_inetx
}  // namespace esphome
