// Host unit tests for the CBOR codec and TruMessageV3 framing.
// Reference vectors were generated with Python cbor2 using the builder of
// https://github.com/daaaaan/truma-inetx-ble (validated against real hardware).
//
// Build & run:  ./tests/run_tests.sh

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "../components/truma_inetx/cbor.h"
#include "../components/truma_inetx/frame.h"

using namespace esphome::truma_inetx;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    checks++;                                                          \
    if (!(cond)) {                                                     \
      failures++;                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                  \
  } while (0)

static std::vector<uint8_t> unhex(const char *hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    unsigned v;
    std::sscanf(hex + i, "%2x", &v);
    out.push_back(static_cast<uint8_t>(v));
  }
  return out;
}

static std::string tohex(const std::vector<uint8_t> &data) {
  std::string s;
  char buf[3];
  for (auto b : data) {
    std::snprintf(buf, sizeof(buf), "%02x", b);
    s += buf;
  }
  return s;
}

static void check_eq_hex(const std::vector<uint8_t> &actual, const char *expected, const char *what) {
  checks++;
  std::string a = tohex(actual);
  if (a != expected) {
    failures++;
    std::printf("FAIL %s\n  expected %s\n  actual   %s\n", what, expected, a.c_str());
  }
}

static void test_builders() {
  check_eq_hex(build_registration(0x0500), "ffff00051200010000000000000000000142a1627076820501", "registration");
  check_eq_hex(build_subscribe(0x0501, {"AirCooling", "RoomClimate"}),
               "000001052700030000000000000000000200a162746e826a416972436f6f6c696e676b526f6f6d436c696d617465",
               "subscribe");
  check_eq_hex(build_write(0x0501, 0x0101, "RoomClimate", "Mode", cbor::Value::make_int(3)),
               "010101052a00030000000000000000000100a462746e6b526f6f6d436c696d61746562706e644d6f646561760362696400",
               "write int");
  check_eq_hex(build_write(0x0501, 0x0202, "AirCooling", "TgtTemp", cbor::Value::make_int(-15)),
               "020201052c00030000000000000000000100a462746e6a416972436f6f6c696e6762706e6754677454656d7061762e62696400",
               "write negative");
  check_eq_hex(build_write(0x0501, 0x0101, "MobileIdentity", "Muid",
                           cbor::Value::make_text("0F2B6C1A-2D3E-4F50-8A1B-2C3D4E5F6A7B")),
               "010101055200030000000000000000000100a462746e6e4d6f62696c654964656e7469747962706e644d75696461767824304632"
               "42364331412d324433452d344635302d384131422d32433344344535463641374262696400",
               "write string");
  check_eq_hex(build_write_map(0x0501, 0x0101, "LastMessage", 1),
               "010101051900030000000000000000000100a16b4c6173744d65737361676501", "LastMessage");
  check_eq_hex(build_param_discovery(0x0501, 0x0202), "020201050b00030000000000000000000400", "param discovery");
}

static void test_parse_frames() {
  auto info = unhex("010502022600030000000000000000000000a362746e6a416972436f6f6c696e6762706e6454656d70617618f5");
  CHECK(expected_frame_length(info.data(), info.size()) == info.size());
  Frame f;
  CHECK(parse_frame(info.data(), info.size(), f));
  CHECK(f.dest == 0x0501);
  CHECK(f.src == 0x0202);
  CHECK(f.control == CTRL_MBP);
  CHECK(f.sub_type == MBP_INFO);
  CHECK(f.cbor.is_map());
  CHECK(f.cbor.get("tn") && f.cbor.get("tn")->str == "AirCooling");
  CHECK(f.cbor.get("pn") && f.cbor.get("pn")->str == "Temp");
  CHECK(f.cbor.get("v") && f.cbor.get("v")->as_int() == 245);

  auto reg = unhex("000501011400010000000000000000000242a16461646472190501");
  CHECK(parse_frame(reg.data(), reg.size(), f));
  CHECK(f.control == CTRL_REGISTRATION && f.sub_type == REG_RESPONSE && f.corr_id == 0x42);
  CHECK(f.cbor.get("addr") && f.cbor.get("addr")->as_int() == 1281);

  auto disc = unhex(
      "010502029a00030000000000000000008400a166746f7069637381a262746e6a416972436f6f6c696e676a706172616d657465727382a862"
      "706e644d6f6465617600647479706501647065726d0365617661696c01636d696e00636d61780164656e756d82a3616e67434f4d464f5254"
      "6161f5617600a3616e64464153546161f5617601a462706e6754677454656d70617618dc636d696e18a0636d617819012c");
  CHECK(expected_frame_length(disc.data(), disc.size()) == disc.size());
  CHECK(parse_frame(disc.data(), disc.size(), f));
  CHECK(f.sub_type == MBP_PARAM_DISCOVERY_RESPONSE);
  const cbor::Value *topics = f.cbor.get("topics");
  CHECK(topics && topics->is_array() && topics->items.size() == 1);
  if (topics && topics->items.size() == 1) {
    const auto &t = topics->items[0];
    CHECK(t.get("tn") && t.get("tn")->str == "AirCooling");
    const cbor::Value *params = t.get("parameters");
    CHECK(params && params->items.size() == 2);
    if (params && params->items.size() == 2) {
      CHECK(params->items[1].get("v")->as_int() == 220);
      CHECK(params->items[1].get("max")->as_int() == 300);
      const cbor::Value *en = params->items[0].get("enum");
      CHECK(en && en->items.size() == 2 && en->items[1].get("n")->str == "FAST");
      CHECK(en && en->items[1].get("a")->type == cbor::Type::BOOL && en->items[1].get("a")->boolean);
    }
  }

  // too short
  CHECK(!parse_frame(info.data(), 10, f));
  CHECK(expected_frame_length(info.data(), 5) == 0);
}

static void test_decoder_types() {
  cbor::Value v;
  auto ints = unhex("a361761a6553f100616e39012b616219ffff");
  CHECK(cbor::decode(ints.data(), ints.size(), v));
  CHECK(v.get("v")->as_int() == 1700000000);
  CHECK(v.get("n")->as_int() == -300);
  CHECK(v.get("b")->as_int() == 65535);

  auto floats = unhex("86fb3ff8000000000000fb4028b8d4fdf3b646fb3fb999999999999afbc000000000000000f5f6");
  CHECK(cbor::decode(floats.data(), floats.size(), v));
  CHECK(v.is_array() && v.items.size() == 6);
  CHECK(std::fabs(v.items[0].as_double() - 1.5) < 1e-12);
  CHECK(std::fabs(v.items[1].as_double() - 12.361) < 1e-12);
  CHECK(v.items[1].as_int() == 12);
  CHECK(v.items[3].as_double() == -2.0);
  CHECK(v.items[4].type == cbor::Type::BOOL && v.items[4].as_int() == 1);
  CHECK(v.items[5].type == cbor::Type::NIL);

  auto half = unhex("f93e00");
  CHECK(cbor::decode(half.data(), half.size(), v) && v.as_double() == 1.5);
  auto single = unhex("fa41460000");  // 12.375f
  CHECK(cbor::decode(single.data(), single.size(), v) && v.as_double() == 12.375);

  // indefinite-length map and text
  auto indef = unhex("bf616101626262" "7f6261626163ff" "ff");
  CHECK(cbor::decode(indef.data(), indef.size(), v));
  CHECK(v.is_map() && v.map_size() == 2);
  CHECK(v.get("a")->as_int() == 1);
  CHECK(v.get("bb") && v.get("bb")->str == "abc");

  // tagged value keeps the inner item
  auto tagged = unhex("c11a6553f100");
  CHECK(cbor::decode(tagged.data(), tagged.size(), v) && v.as_int() == 1700000000);

  // to_string
  auto map = unhex("a362746e6a416972436f6f6c696e6762706e6454656d70617618f5");
  CHECK(cbor::decode(map.data(), map.size(), v));
  CHECK(v.to_string() == "{\"tn\":\"AirCooling\",\"pn\":\"Temp\",\"v\":245}");
}

static void test_decoder_errors() {
  cbor::Value v;
  auto truncated = unhex("a362746e6a416972436f6f6c");
  CHECK(!cbor::decode(truncated.data(), truncated.size(), v));
  CHECK(!v.valid());
  auto huge_len = unhex("7b7fffffffffffffff");
  CHECK(!cbor::decode(huge_len.data(), huge_len.size(), v));
  auto huge_array = unhex("9b00000000ffffffff");
  CHECK(!cbor::decode(huge_array.data(), huge_array.size(), v));
  std::vector<uint8_t> deep(40, 0x81);  // 40 nested arrays
  deep.push_back(0x01);
  CHECK(!cbor::decode(deep.data(), deep.size(), v));
  auto bad_break = unhex("ff");
  CHECK(!cbor::decode(bad_break.data(), bad_break.size(), v));
  auto odd_indef_map = unhex("bf6161ff");
  CHECK(!cbor::decode(odd_indef_map.data(), odd_indef_map.size(), v));
  CHECK(!cbor::decode(nullptr, 0, v));
}

static void test_encoder_roundtrip() {
  std::vector<uint8_t> buf;
  cbor::Encoder enc(buf);
  enc.map(3);
  enc.text("big");
  enc.integer(5000000000LL);
  enc.text("neg");
  enc.integer(-70000);
  enc.text(std::string(30, 'x'));
  enc.array(25);
  for (int i = 0; i < 25; i++)
    enc.integer(i * 1000);
  cbor::Value v;
  size_t used = 0;
  CHECK(cbor::decode(buf.data(), buf.size(), v, &used));
  CHECK(used == buf.size());
  CHECK(v.get("big")->as_int() == 5000000000LL);
  CHECK(v.get("neg")->as_int() == -70000);
  const cbor::Value *arr = v.get(std::string(30, 'x').c_str());
  CHECK(arr && arr->items.size() == 25 && arr->items[24].as_int() == 24000);

  // re-encode a decoded value and compare
  std::vector<uint8_t> again;
  cbor::Encoder enc2(again);
  enc2.value(v);
  CHECK(again == buf);
}

static void test_fuzz() {
  // Random and mutated inputs must never crash (run with -fsanitize=address,undefined).
  std::mt19937 rng(1234);
  auto seed = unhex(
      "010502029a00030000000000000000008400a166746f7069637381a262746e6a416972436f6f6c696e676a706172616d657465727382a862"
      "706e644d6f6465617600647479706501647065726d0365617661696c01636d696e00636d61780164656e756d82a3616e67434f4d464f5254"
      "6161f5617600a3616e64464153546161f5617601a462706e6754677454656d70617618dc636d696e18a0636d617819012c");
  for (int iter = 0; iter < 200000; iter++) {
    std::vector<uint8_t> data;
    if (iter % 2 == 0) {
      data = seed;
      int flips = 1 + rng() % 4;
      for (int i = 0; i < flips; i++)
        data[rng() % data.size()] = static_cast<uint8_t>(rng());
      data.resize(rng() % (data.size() + 1));
    } else {
      data.resize(rng() % 64);
      for (auto &b : data)
        b = static_cast<uint8_t>(rng());
    }
    Frame f;
    parse_frame(data.data(), data.size(), f);
    (void) f.cbor.to_string(100);
    cbor::Value v;
    cbor::decode(data.data(), data.size(), v);
    (void) expected_frame_length(data.data(), data.size());
  }
  checks++;
}

int main() {
  test_builders();
  test_parse_frames();
  test_decoder_types();
  test_decoder_errors();
  test_encoder_roundtrip();
  test_fuzz();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
