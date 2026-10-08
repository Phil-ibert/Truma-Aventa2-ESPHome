#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#ifdef USE_ESP32

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cbor.h"
#include "frame.h"

namespace esphome {
namespace truma_inetx {

namespace espbt = esphome::esp32_ble_tracker;

/// Session state, from BLE link up to a fully initialised iNet X session.
enum class SessionState : uint8_t {
  DISCONNECTED = 0,
  CONNECTING,   // BLE link up, waiting for service discovery
  SECURING,     // pairing / encryption in progress
  SUBSCRIBING,  // enabling GATT notifications
  REGISTERING,  // waiting for the address assignment
  INITIALISING, // topic subscription, identity, parameter discovery
  READY,
  FAILED,       // the device does not expose the expected GATT layout
};

const char *session_state_to_string(SessionState state);

/// Stored value of one "Topic.Parameter".
struct ParamEntry {
  cbor::Value value;
  uint16_t source{0};
};

class TrumaInetX : public Component, public ble_client::BLEClientNode, public espbt::ESPBTDeviceListener {
 public:
  // ---- ESPHome lifecycle --------------------------------------------------
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
  /// Advertisement listener: finds Truma devices, follows address changes, pairing helper.
  bool parse_device(const espbt::ESPBTDevice &device) override;

  // ---- configuration (called from generated code) --------------------------
  void set_user_name(const std::string &name) { this->user_name_ = name; }
  void set_identity(const std::string &muid, const std::string &uuid) {
    this->muid_ = muid;
    this->uuid_ = uuid;
  }
  void set_send_identity(bool send) { this->send_identity_ = send; }
  void set_encryption(bool encryption) { this->encryption_ = encryption; }
  void set_pin(uint32_t pin) {
    this->pin_ = pin;
    this->has_pin_ = true;
  }
  void add_topic(const std::string &topic) { this->topics_.push_back(topic); }
  void add_discovery_address(uint16_t address) { this->discovery_addresses_.push_back(address); }
  void set_default_destination(uint16_t address) { this->default_destination_ = address; }
  void set_topic_destination(const std::string &topic, uint16_t address) { this->destinations_[topic] = address; }
  void set_auto_discovery(bool enabled) { this->auto_discovery_ = enabled; }
  void set_optimistic(bool optimistic) { this->optimistic_ = optimistic; }
  void set_log_frames(bool log_frames) { this->log_frames_ = log_frames; }
  void set_frame_delay(uint32_t ms) { this->frame_delay_ = ms; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_remember_address(bool remember) { this->remember_address_ = remember; }
  void set_log_advertisements(bool log) { this->log_advertisements_ = log; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
#endif

  // ---- public API for entities and lambdas ----------------------------------
  /// Queue a parameter write. `destination` 0 = automatic routing. Returns false if not connected.
  bool write(const std::string &topic, const std::string &param, const cbor::Value &value, uint16_t destination = 0);
  bool write_int(const std::string &topic, const std::string &param, int64_t value, uint16_t destination = 0) {
    return this->write(topic, param, cbor::Value::make_int(value), destination);
  }
  bool write_string(const std::string &topic, const std::string &param, const std::string &value,
                    uint16_t destination = 0) {
    return this->write(topic, param, cbor::Value::make_text(value), destination);
  }
  /// Ask every known device for all its parameters again.
  void refresh();
  /// Log every parameter currently known (handy from a Home Assistant action).
  void dump_parameters();
  /// First pairing helper: pick the nearest Truma device, connect to its current address and bond.
  void start_pairing();
  /// Forget the address remembered after bonding (falls back to the YAML mac_address).
  void forget_remembered_address();

  /// Last known value of Topic.Parameter, or nullptr.
  const cbor::Value *get_value(const std::string &topic, const std::string &param) const;

  bool is_ready() const { return this->state_ == SessionState::READY; }
  SessionState get_state() const { return this->state_; }
  uint16_t get_assigned_address() const { return this->assigned_addr_; }

  /// Called with every new value of Topic.Parameter.
  void register_listener(const std::string &topic, const std::string &param,
                         std::function<void(const cbor::Value &)> &&callback);
  /// Called on every session state change.
  void add_on_state_callback(std::function<void(SessionState)> &&callback) {
    this->state_callbacks_.add(std::move(callback));
  }

 protected:
  struct OutFrame {
    std::vector<uint8_t> data;
    uint32_t delay_after{0};
  };
  struct GattWrite {
    uint16_t handle;
    std::vector<uint8_t> data;
    bool response;
  };
  struct Listener {
    std::string topic;
    std::string param;
    std::function<void(const cbor::Value &)> callback;
  };
  enum class TxState : uint8_t { IDLE, WAIT_READY, WAIT_ACK };

  // connection / GATT
  void set_state_(SessionState state);
  void reset_session_();
  void on_services_discovered_();
  void register_notifications_();
  void dump_gatt_database_();
  void queue_gatt_write_(uint16_t handle, std::vector<uint8_t> data, bool response);
  void process_gatt_queue_(uint32_t now);

  // transport
  void queue_frame_(std::vector<uint8_t> &&data, uint32_t delay_after = 0);
  void process_tx_(uint32_t now);
  void send_tx_data_(uint32_t now);
  void finish_tx_(uint32_t now);
  void on_cmd_notify_(const uint8_t *data, uint16_t len);
  void on_data_notify_(const uint8_t *data, uint16_t len);
  void process_rx_frame_(const uint8_t *data, size_t len);

  // protocol session
  void start_session_();
  void continue_initialisation_();
  void queue_param_discovery_(uint16_t address);
  void handle_frame_(const Frame &frame);
  void handle_info_(uint16_t source, const cbor::Value &value);
  void handle_discovery_response_(uint16_t source, const cbor::Value &value);
  void update_param_(uint16_t source, const std::string &topic, const std::string &param, const cbor::Value &value,
                     bool from_device);
  uint16_t resolve_destination_(const std::string &topic) const;
  void log_frame_(const char *direction, const uint8_t *data, size_t len);

  // addresses / bonding
  struct StoredAddress {
    uint32_t magic;
    uint64_t configured;  // YAML mac_address this record belongs to
    uint64_t bonded;      // address Bluedroid reports for the bonded device after a reboot
  };
  bool is_truma_advertisement_(const espbt::ESPBTDevice &device);
  void retarget_(uint64_t address, const char *reason);
  void remember_bond_(const uint8_t *peer);
  void store_address_(uint64_t address);

  // configuration
  std::string user_name_{"ESPHome"};
  std::string muid_;
  std::string uuid_;
  bool send_identity_{true};
  bool encryption_{true};
  bool has_pin_{false};
  uint32_t pin_{0};
  std::vector<std::string> topics_;
  std::vector<uint16_t> discovery_addresses_;
  uint16_t default_destination_{ADDR_PANEL};
  std::map<std::string, uint16_t> destinations_;
  bool auto_discovery_{true};
  bool optimistic_{true};
  bool log_frames_{false};
  uint32_t frame_delay_{100};
  std::string device_name_;
  bool remember_address_{true};
  bool log_advertisements_{true};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

  // GATT
  SessionState state_{SessionState::DISCONNECTED};
  uint16_t cmd_handle_{0};
  uint16_t data_w_handle_{0};
  uint16_t data_r_handle_{0};
  uint16_t mtu_{23};
  uint8_t pending_notify_{0};
  uint32_t state_deadline_{0};
  std::deque<GattWrite> gatt_queue_;
  bool gatt_busy_{false};
  uint16_t gatt_busy_handle_{0};
  uint32_t gatt_busy_since_{0};

  // transport
  std::deque<OutFrame> tx_queue_;
  OutFrame tx_current_;
  TxState tx_state_{TxState::IDLE};
  uint32_t tx_deadline_{0};
  uint32_t next_tx_at_{0};
  std::vector<uint8_t> rx_buffer_;
  size_t rx_expected_{0};
  uint32_t rx_started_{0};

  // protocol
  uint16_t assigned_addr_{ADDR_APP_DEFAULT};
  bool init_queued_{false};
  std::map<std::string, ParamEntry> params_;
  std::map<std::string, uint16_t> learned_destinations_;
  std::set<uint16_t> probed_addresses_;
  std::vector<Listener> listeners_;
  CallbackManager<void(SessionState)> state_callbacks_;

  // addresses / bonding
  uint64_t configured_address_{0};
  uint64_t pending_target_{0};
  ESPPreferenceObject address_pref_;
  std::set<uint64_t> seen_addresses_;
  bool pairing_mode_{false};
  uint32_t pairing_deadline_{0};
  uint64_t pairing_candidate_{0};
  int pairing_rssi_{-127};
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
