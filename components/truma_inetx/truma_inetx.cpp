#include "truma_inetx.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx";

static const uint32_t SECURING_TIMEOUT_MS = 20000;
static const uint32_t REGISTRATION_TIMEOUT_MS = 10000;
static const uint32_t TRANSPORT_TIMEOUT_MS = 3000;
static const uint32_t GATT_WRITE_TIMEOUT_MS = 5000;
static const uint32_t RX_REASSEMBLY_TIMEOUT_MS = 2000;
static const uint32_t SUBSCRIBE_BATCH_DELAY_MS = 250;
static const uint32_t IDENTITY_DELAY_MS = 300;
static const uint32_t DISCOVERY_DELAY_MS = 1000;
static const size_t MAX_TOPICS_PER_SUBSCRIBE = 10;
static const size_t MAX_FRAME_SIZE = 4096;
static const size_t MAX_TX_QUEUE = 100;
static const uint32_t PAIRING_SCAN_MS = 8000;
static const size_t MAX_LOGGED_ADVERTISERS = 32;
static const uint32_t ADDRESS_PREF_MAGIC = 0x54524D41;  // "TRMA"

static void format_address(uint64_t address, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned) ((address >> 40) & 0xFF),
           (unsigned) ((address >> 32) & 0xFF), (unsigned) ((address >> 24) & 0xFF),
           (unsigned) ((address >> 16) & 0xFF), (unsigned) ((address >> 8) & 0xFF), (unsigned) (address & 0xFF));
}

static uint64_t bda_to_u64(const uint8_t *bda) {
  uint64_t address = 0;
  for (int i = 0; i < 6; i++)
    address = (address << 8) | bda[i];
  return address;
}

/// Human readable kind of a BLE address (Core spec Vol 6 Part B 1.3).
static const char *address_kind(esp_ble_addr_type_t type, uint64_t address) {
  switch (type) {
    case BLE_ADDR_TYPE_PUBLIC:
      return "public (fixed)";
    case BLE_ADDR_TYPE_RANDOM:
      switch ((address >> 46) & 0x3) {
        case 0x3:
          return "random static";
        case 0x1:
          return "resolvable private (RPA, rotates)";
        case 0x0:
          return "non-resolvable private (rotates)";
        default:
          return "random (reserved)";
      }
    case BLE_ADDR_TYPE_RPA_PUBLIC:
    case BLE_ADDR_TYPE_RPA_RANDOM:
      return "resolved private";
    default:
      return "unknown";
  }
}

static std::string lower(std::string s) {
  for (auto &c : s)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

const char *session_state_to_string(SessionState state) {
  switch (state) {
    case SessionState::DISCONNECTED:
      return "disconnected";
    case SessionState::CONNECTING:
      return "connecting";
    case SessionState::SECURING:
      return "securing";
    case SessionState::SUBSCRIBING:
      return "subscribing";
    case SessionState::REGISTERING:
      return "registering";
    case SessionState::INITIALISING:
      return "initialising";
    case SessionState::READY:
      return "ready";
    case SessionState::FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TrumaInetX::setup() {
  this->reset_session_();
  if (this->poll_interval_ > 0)
    this->set_interval("poll", this->poll_interval_, [this]() { this->poll_(); });
  this->configured_address_ = this->parent()->get_address();
  this->address_pref_ = global_preferences->make_preference<StoredAddress>(fnv1_hash("truma_inetx_address"), true);
  if (!this->remember_address_)
    return;
  StoredAddress stored{};
  if (this->address_pref_.load(&stored) && stored.magic == ADDRESS_PREF_MAGIC &&
      stored.configured == this->configured_address_ && stored.bonded != 0 &&
      stored.bonded != this->configured_address_) {
    char a[18], b[18];
    format_address(stored.bonded, a);
    format_address(this->configured_address_, b);
    ESP_LOGI(TAG, "Using the address remembered after bonding: %s (mac_address in YAML: %s)", a, b);
    this->parent()->set_address(stored.bonded);
  }
}

void TrumaInetX::dump_config() {
  ESP_LOGCONFIG(TAG, "Truma iNet X (BLE):");
  ESP_LOGCONFIG(TAG, "  Encryption/pairing: %s, PIN configured: %s", YESNO(this->encryption_), YESNO(this->has_pin_));
  ESP_LOGCONFIG(TAG, "  Identity: user '%s', muid %s, uuid %s (sent: %s)", this->user_name_.c_str(),
                this->muid_.c_str(), this->uuid_.c_str(), YESNO(this->send_identity_));
  ESP_LOGCONFIG(TAG, "  Topics subscribed: %u", (unsigned) this->topics_.size());
  ESP_LOGCONFIG(TAG, "  Default destination: 0x%04X, auto discovery: %s, optimistic: %s",
                this->default_destination_, YESNO(this->auto_discovery_), YESNO(this->optimistic_));
  for (auto address : this->discovery_addresses_)
    ESP_LOGCONFIG(TAG, "  Parameter discovery address: 0x%04X", address);
  for (const auto &it : this->destinations_)
    ESP_LOGCONFIG(TAG, "  Destination for topic %s: 0x%04X", it.first.c_str(), it.second);
  ESP_LOGCONFIG(TAG, "  Frame logging: %s, advertisement logging: %s", YESNO(this->log_frames_),
                YESNO(this->log_advertisements_));
  ESP_LOGCONFIG(TAG, "  Remember bonded address: %s", YESNO(this->remember_address_));
  if (this->poll_interval_ > 0) {
    ESP_LOGCONFIG(TAG, "  Poll interval: %u s", (unsigned) (this->poll_interval_ / 1000));
  } else {
    ESP_LOGCONFIG(TAG, "  Poll interval: never");
  }
  if (!this->device_name_.empty())
    ESP_LOGCONFIG(TAG, "  Follow device name: '%s'", this->device_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Session state: %s", session_state_to_string(this->state_));
}

void TrumaInetX::loop() {
  const uint32_t now = millis();

  switch (this->state_) {
    case SessionState::SECURING:
      if ((int32_t) (now - this->state_deadline_) > 0) {
        ESP_LOGW(TAG, "Pairing/encryption did not complete in time, continuing without it");
        this->register_notifications_();
      }
      break;
    case SessionState::REGISTERING:
      if ((int32_t) (now - this->state_deadline_) > 0) {
        ESP_LOGW(TAG, "No registration response, continuing with address 0x%04X", this->assigned_addr_);
        this->continue_initialisation_();
      }
      break;
    case SessionState::INITIALISING:
      if (this->tx_queue_.empty() && this->tx_state_ == TxState::IDLE) {
        ESP_LOGI(TAG, "Session ready (address 0x%04X, %u parameters known)", this->assigned_addr_,
                 (unsigned) this->params_.size());
        this->set_state_(SessionState::READY);
        // Without bonding (no AUTH_CMPL), still remember a working address found by pairing / name.
        if (!this->parent()->is_paired())
          this->store_address_(this->parent()->get_address());
      }
      break;
    default:
      break;
  }

  if (this->pairing_mode_ && (int32_t) (now - this->pairing_deadline_) > 0) {
    this->pairing_mode_ = false;
    this->parent()->set_auto_connect(true);
    if (this->pairing_candidate_ == 0) {
      ESP_LOGW(TAG, "Pairing: no Truma device heard. Is the Aventa powered and its Bluetooth on?");
    } else {
      char a[18];
      format_address(this->pairing_candidate_, a);
      ESP_LOGI(TAG, "Pairing: nearest Truma device is %s (RSSI %d dBm), connecting and bonding", a,
               this->pairing_rssi_);
      this->retarget_(this->pairing_candidate_, "pairing");
    }
  }

  if (this->has_pending_target_ && this->parent()->state() == espbt::ClientState::IDLE) {
    this->parent()->set_address(this->pending_target_);
    this->has_pending_target_ = false;
  }

  if (!this->rx_buffer_.empty() && (now - this->rx_started_) > RX_REASSEMBLY_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Incomplete frame dropped (%u of %u bytes)", (unsigned) this->rx_buffer_.size(),
             (unsigned) this->rx_expected_);
    this->rx_buffer_.clear();
    this->rx_expected_ = 0;
  }

  this->process_gatt_queue_(now);
  this->process_tx_(now);
}

void TrumaInetX::set_state_(SessionState state) {
  if (state == this->state_)
    return;
  ESP_LOGD(TAG, "Session state: %s -> %s", session_state_to_string(this->state_), session_state_to_string(state));
  this->state_ = state;
  this->state_callbacks_.call(state);
}

void TrumaInetX::reset_session_() {
  this->cmd_handle_ = 0;
  this->data_w_handle_ = 0;
  this->data_r_handle_ = 0;
  this->pending_notify_ = 0;
  this->gatt_queue_.clear();
  this->gatt_busy_ = false;
  this->tx_queue_.clear();
  this->tx_current_ = OutFrame();
  this->tx_state_ = TxState::IDLE;
  this->next_tx_at_ = 0;
  this->rx_buffer_.clear();
  this->rx_expected_ = 0;
  this->assigned_addr_ = ADDR_APP_DEFAULT;
  this->init_queued_ = false;
  this->learned_destinations_.clear();
  this->probed_addresses_.clear();
}

// ---------------------------------------------------------------------------
// BLE events
// ---------------------------------------------------------------------------

void TrumaInetX::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                     esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_CONNECT_EVT:
      // The parent starts MTU negotiation here; CFG_MTU_EVT may arrive before OPEN_EVT.
      this->mtu_ = 23;
      break;

    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Connected to %s", this->parent()->address_str());
        this->reset_session_();
        this->set_state_(SessionState::CONNECTING);
      }
      break;

    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK) {
        this->mtu_ = param->cfg_mtu.mtu;
        ESP_LOGD(TAG, "MTU negotiated: %u", this->mtu_);
      }
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
      this->on_services_discovered_();
      break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      uint16_t handle = param->reg_for_notify.handle;
      if (handle == 0 || (handle != this->cmd_handle_ && handle != this->data_r_handle_))
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK)
        ESP_LOGW(TAG, "Notification registration failed for handle 0x%04X (status %d)", handle,
                 param->reg_for_notify.status);
      if (this->pending_notify_ > 0)
        this->pending_notify_--;
      if (this->pending_notify_ == 0 && this->state_ == SessionState::SUBSCRIBING) {
        // The parent may now release its GATT cache: we only use the stored handles from here on.
        this->node_state = espbt::ClientState::ESTABLISHED;
        this->start_session_();
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.conn_id != this->parent()->get_conn_id())
        break;
      if (param->notify.handle == this->cmd_handle_) {
        this->on_cmd_notify_(param->notify.value, param->notify.value_len);
      } else if (param->notify.handle == this->data_r_handle_) {
        this->on_data_notify_(param->notify.value, param->notify.value_len);
      }
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.conn_id != this->parent()->get_conn_id())
        break;
      if (this->gatt_busy_ && param->write.handle == this->gatt_busy_handle_)
        this->gatt_busy_ = false;
      if (param->write.status != ESP_GATT_OK) {
        if (param->write.status == ESP_GATT_INSUF_AUTHENTICATION || param->write.status == ESP_GATT_INSUF_ENCRYPTION) {
          ESP_LOGW(TAG, "Write refused by the device: pairing required (check `encryption` and `pin`)");
        } else {
          ESP_LOGW(TAG, "Write to handle 0x%04X failed, status %d", param->write.handle, param->write.status);
        }
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      if (this->state_ != SessionState::DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from %s", this->parent()->address_str());
        this->reset_session_();
        this->set_state_(SessionState::DISCONNECTED);
      }
      break;

    default:
      break;
  }
}

void TrumaInetX::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (!this->parent()->check_addr(param->ble_security.auth_cmpl.bd_addr))
        return;
      if (param->ble_security.auth_cmpl.success) {
        ESP_LOGI(TAG, "Link encrypted (pairing/bonding OK)");
        this->remember_bond_(param->ble_security.auth_cmpl.bd_addr);
      } else {
        ESP_LOGW(TAG,
                 "Pairing failed (reason 0x%02X). Wrong PIN? Remove the bond and retry, "
                 "or put the Aventa in pairing mode.",
                 param->ble_security.auth_cmpl.fail_reason);
      }
      if (this->state_ == SessionState::SECURING)
        this->register_notifications_();
      break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      if (!this->parent()->check_addr(param->ble_security.ble_req.bd_addr))
        return;
      if (this->has_pin_) {
        ESP_LOGI(TAG, "Device requests a PIN, replying with the configured one");
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, this->pin_);
      } else {
        ESP_LOGW(TAG, "Device requests a PIN: set `pin:` in the truma_inetx configuration");
      }
      break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      if (!this->parent()->check_addr(param->ble_security.key_notif.bd_addr))
        return;
      ESP_LOGW(TAG, "Passkey to enter on the device: %06u", (unsigned) param->ble_security.key_notif.passkey);
      break;

    case ESP_GAP_BLE_NC_REQ_EVT:
      if (!this->parent()->check_addr(param->ble_security.key_notif.bd_addr))
        return;
      ESP_LOGI(TAG, "Numeric comparison requested (%06u), accepting",
               (unsigned) param->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
      break;

    default:
      break;
  }
}

bool TrumaInetX::find_characteristic_(const char *uuid, uint16_t *handle, uint8_t *properties) {
  const auto char_uuid = espbt::ESPBTUUID::from_raw(uuid);
  // 1. known services (Aventa 2, then iNet X panel)
  for (const char *service : {SERVICE_UUID_AVENTA, SERVICE_UUID_PANEL}) {
    auto *chr = this->parent()->get_characteristic(espbt::ESPBTUUID::from_raw(service), char_uuid);
    if (chr != nullptr) {
      *handle = chr->handle;
      *properties = chr->properties;
      return true;
    }
  }
  // 2. anywhere in the GATT database (future devices may use yet another service)
  esp_gattc_char_elem_t result;
  uint16_t count = 1;
  esp_gatt_status_t status =
      esp_ble_gattc_get_char_by_uuid(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), 0x0001, 0xFFFF,
                                     char_uuid.get_uuid(), &result, &count);
  if (status == ESP_GATT_OK && count > 0) {
    *handle = result.char_handle;
    *properties = result.properties;
    return true;
  }
  return false;
}

void TrumaInetX::on_services_discovered_() {
  uint8_t cmd_props = 0, data_w_props = 0, data_r_props = 0;
  bool cmd = this->find_characteristic_(CHAR_CMD_UUID, &this->cmd_handle_, &cmd_props);
  bool data_w = this->find_characteristic_(CHAR_DATA_W_UUID, &this->data_w_handle_, &data_w_props);
  bool data_r = this->find_characteristic_(CHAR_DATA_R_UUID, &this->data_r_handle_, &data_r_props);

  if (!cmd || !data_w || !data_r) {
    ESP_LOGE(TAG, "Truma iNet X characteristics not found (cmd=%s data_w=%s data_r=%s). GATT layout:", YESNO(cmd),
             YESNO(data_w), YESNO(data_r));
    this->dump_gatt_database_();
    this->cmd_handle_ = this->data_w_handle_ = this->data_r_handle_ = 0;
    this->set_state_(SessionState::FAILED);
    this->node_state = espbt::ClientState::ESTABLISHED;  // let the parent release its cache
    return;
  }
  if (this->log_frames_)
    this->dump_gatt_database_();

  ESP_LOGI(TAG, "iNet X characteristics found: CMD 0x%04X (props 0x%02X), DATA_W 0x%04X (props 0x%02X), "
                "DATA_R 0x%04X (props 0x%02X)",
           this->cmd_handle_, cmd_props, this->data_w_handle_, data_w_props, this->data_r_handle_, data_r_props);

  if (this->encryption_ && this->parent()->is_paired()) {
    ESP_LOGD(TAG, "Link already encrypted (device-initiated security)");
    this->register_notifications_();
  } else if (this->encryption_) {
    this->set_state_(SessionState::SECURING);
    this->state_deadline_ = millis() + SECURING_TIMEOUT_MS;
    esp_err_t err = this->parent()->pair();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Could not start pairing (err %d), continuing without encryption", err);
      this->register_notifications_();
    }
  } else {
    this->register_notifications_();
  }
}

void TrumaInetX::register_notifications_() {
  this->set_state_(SessionState::SUBSCRIBING);
  this->pending_notify_ = 0;
  // Do NOT subscribe to FC314004 (CMD_ALT): it breaks the transport.
  // Raw ESP-IDF call (works on every ESPHome version). The parent keeps its GATT cache
  // because this node only reports ESTABLISHED once both registrations completed.
  for (uint16_t handle : {this->cmd_handle_, this->data_r_handle_}) {
    esp_err_t err =
        esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(), this->parent()->get_remote_bda(), handle);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "register_for_notify(0x%04X) failed: %d", handle, err);
    } else {
      this->pending_notify_++;
    }
  }
  if (this->pending_notify_ == 0) {
    ESP_LOGE(TAG, "Could not enable notifications, disconnecting");
    this->set_state_(SessionState::FAILED);
    this->node_state = espbt::ClientState::ESTABLISHED;
    this->parent()->disconnect();
  }
}

void TrumaInetX::dump_gatt_database_() {
  auto *client = this->parent();
  uint16_t count = 0;
  esp_gatt_status_t status = esp_ble_gattc_get_attr_count(client->get_gattc_if(), client->get_conn_id(),
                                                          ESP_GATT_DB_ALL, 0x0001, 0xFFFF, 0, &count);
  if (status != ESP_GATT_OK || count == 0) {
    ESP_LOGW(TAG, "  (GATT database unavailable, status %d)", status);
    return;
  }
  if (count > 64)
    count = 64;
  std::vector<esp_gattc_db_elem_t> db(count);
  status = esp_ble_gattc_get_db(client->get_gattc_if(), client->get_conn_id(), 0x0001, 0xFFFF, db.data(), &count);
  if (status != ESP_GATT_OK) {
    ESP_LOGW(TAG, "  (esp_ble_gattc_get_db failed, status %d)", status);
    return;
  }
  char uuid_buf[espbt::UUID_STR_LEN];
  for (uint16_t i = 0; i < count; i++) {
    const auto &e = db[i];
    espbt::ESPBTUUID::from_uuid(e.uuid).to_str(uuid_buf);
    const char *kind = "attribute";
    switch (e.type) {
      case ESP_GATT_DB_PRIMARY_SERVICE:
        kind = "service";
        break;
      case ESP_GATT_DB_SECONDARY_SERVICE:
        kind = "secondary service";
        break;
      case ESP_GATT_DB_CHARACTERISTIC:
        kind = "  characteristic";
        break;
      case ESP_GATT_DB_DESCRIPTOR:
        kind = "    descriptor";
        break;
      default:
        break;
    }
    ESP_LOGI(TAG, "  %s %s handle 0x%04X props 0x%02X", kind, uuid_buf, e.attribute_handle, e.properties);
  }
}

// ---------------------------------------------------------------------------
// Advertisements, address changes and bonding
// ---------------------------------------------------------------------------

bool TrumaInetX::is_truma_advertisement_(const espbt::ESPBTDevice &device) {
  if (device.address_uint64() == this->parent()->get_address())
    return true;
  const auto truma_id = espbt::ESPBTUUID::from_uint16(TRUMA_MANUFACTURER_ID);
  for (const auto &data : device.get_manufacturer_datas()) {
    if (data.uuid == truma_id)
      return true;
  }
  if (!device.get_service_uuids().empty()) {
    char buf[espbt::UUID_STR_LEN];
    for (const auto &uuid : device.get_service_uuids()) {
      std::string s = lower(uuid.to_str(buf));
      if (s.rfind("fc31", 0) == 0 && s.find("f3b2-11e8-8eb2-f2801f1b9fd1") != std::string::npos)
        return true;
    }
  }
  const char *raw_name = device.get_name().c_str();
  if (raw_name[0] == '\0')
    return false;
  std::string name = lower(raw_name);
  return name.find("truma") != std::string::npos || name.find("aventa") != std::string::npos ||
         name.find("inet") != std::string::npos;
}

bool TrumaInetX::parse_device(const espbt::ESPBTDevice &device) {
  if (!this->is_truma_advertisement_(device))
    return false;
  const uint64_t address = device.address_uint64();
  const int rssi = device.get_rssi();

  if (this->pairing_mode_ && (this->pairing_candidate_ == 0 || rssi > this->pairing_rssi_)) {
    this->pairing_candidate_ = address;
    this->pairing_rssi_ = rssi;
  }

  // Follow a device whose address changed without bonding, identified by its advertised name.
  if (!this->device_name_.empty() && address != this->parent()->get_address() &&
      this->parent()->state() == espbt::ClientState::IDLE &&
      strcmp(device.get_name().c_str(), this->device_name_.c_str()) == 0) {
    this->retarget_(address, "advertised name match");
  }

  if (this->log_advertisements_ && this->seen_addresses_.size() < MAX_LOGGED_ADVERTISERS &&
      this->seen_addresses_.insert(address).second) {
    char a[18];
    format_address(address, a);
    std::string manufacturer;
    for (const auto &data : device.get_manufacturer_datas()) {
      char id[espbt::UUID_STR_LEN];
      data.uuid.to_str(id);
      manufacturer += std::string(" [") + id + "] " + hex_dump(data.data.data(), data.data.size(), 24);
    }
    ESP_LOGI(TAG, "Truma device heard: %s, %s, RSSI %d dBm, name '%s'%s%s", a,
             address_kind(device.get_address_type(), address), rssi, device.get_name().c_str(),
             manufacturer.empty() ? "" : ", manufacturer data:", manufacturer.c_str());
    if (address == this->parent()->get_address())
      ESP_LOGI(TAG, "  -> this is the configured device");
  }
  return false;  // never consume: the BLE client and other listeners need it too
}

void TrumaInetX::retarget_(uint64_t address, const char *reason) {
  if (address == 0 || address == this->parent()->get_address() ||
      (this->has_pending_target_ && address == this->pending_target_))
    return;
  char a[18], b[18];
  format_address(address, a);
  format_address(this->parent()->get_address(), b);
  ESP_LOGI(TAG, "Switching target address %s -> %s (%s)", b, a, reason);
  // The address may only change while the client is idle: disconnect first if needed.
  this->pending_target_ = address;
  this->has_pending_target_ = true;
  if (this->parent()->state() != espbt::ClientState::IDLE)
    this->parent()->disconnect();
}

void TrumaInetX::start_pairing() {
  ESP_LOGI(TAG, "Pairing: listening %u s for the nearest Truma device...", (unsigned) (PAIRING_SCAN_MS / 1000));
  // Stay disconnected while listening: a connected Aventa usually stops advertising.
  this->parent()->set_auto_connect(false);
  if (this->parent()->state() != espbt::ClientState::IDLE)
    this->parent()->disconnect();
  this->pairing_mode_ = true;
  this->pairing_deadline_ = millis() + PAIRING_SCAN_MS;
  this->pairing_candidate_ = 0;
  this->pairing_rssi_ = -127;
}

void TrumaInetX::forget_remembered_address() {
  StoredAddress empty{};
  this->address_pref_.save(&empty);
  global_preferences->sync();
  ESP_LOGI(TAG, "Remembered address cleared, the YAML mac_address will be used after a restart");
}

void TrumaInetX::store_address_(uint64_t address) {
  if (!this->remember_address_ || address == 0)
    return;
  StoredAddress current{};
  bool loaded = this->address_pref_.load(&current) && current.magic == ADDRESS_PREF_MAGIC &&
                current.configured == this->configured_address_;
  uint64_t effective = loaded && current.bonded != 0 ? current.bonded : this->configured_address_;
  if (address == effective)
    return;
  StoredAddress record{ADDRESS_PREF_MAGIC, this->configured_address_, address};
  this->address_pref_.save(&record);
  global_preferences->sync();
  char a[18];
  format_address(address, a);
  ESP_LOGI(TAG, "Address %s remembered for the next restarts (you may also set it as mac_address)", a);
}

void TrumaInetX::forget_pairing() {
  auto *client = this->parent();
  if (client->get_address() != 0) {
    esp_bd_addr_t bda;
    memcpy(bda, client->get_remote_bda(), sizeof(esp_bd_addr_t));
    esp_err_t err = esp_ble_remove_bond_device(bda);
    char a[18];
    format_address(client->get_address(), a);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Bond with %s removed", a);
    } else {
      ESP_LOGW(TAG, "No bond removed for %s (err %d)", a, err);
    }
  }
  this->forget_remembered_address();
  if (this->configured_address_ != client->get_address()) {
    this->pending_target_ = this->configured_address_;
    this->has_pending_target_ = true;
    if (client->state() != espbt::ClientState::IDLE)
      client->disconnect();
  } else if (client->state() != espbt::ClientState::IDLE) {
    client->disconnect();
  }
}

void TrumaInetX::remember_bond_(const uint8_t *peer) {
  int count = esp_ble_get_bond_device_num();
  if (count <= 0)
    return;
  std::vector<esp_ble_bond_dev_t> bonds(count);
  if (esp_ble_get_bond_device_list(&count, bonds.data()) != ESP_OK)
    return;
  const uint8_t *remote = this->parent()->get_remote_bda();
  for (int i = 0; i < count; i++) {
    const auto &bond = bonds[i];
    const bool has_identity = (bond.bond_key.key_mask & ESP_LE_KEY_PID) != 0;
    const bool match = memcmp(bond.bd_addr, peer, 6) == 0 || memcmp(bond.bd_addr, remote, 6) == 0 ||
                       (has_identity && (memcmp(bond.bond_key.pid_key.static_addr, peer, 6) == 0 ||
                                         memcmp(bond.bond_key.pid_key.static_addr, remote, 6) == 0));
    if (!match)
      continue;
    const uint64_t stored = bda_to_u64(bond.bd_addr);
    char s[18], id[18];
    format_address(stored, s);
    if (has_identity) {
      format_address(bda_to_u64(bond.bond_key.pid_key.static_addr), id);
      ESP_LOGI(TAG, "Bond stored: address %s, identity address %s, IRK received: the ESP32 will resolve its "
                    "rotating private addresses by itself", s, id);
    } else {
      ESP_LOGI(TAG, "Bond stored: address %s (no IRK: the device does not use private addresses)", s);
    }
    this->store_address_(stored);
    return;
  }
  ESP_LOGW(TAG, "Bond not found in the ESP32 bond list after pairing");
}

// ---------------------------------------------------------------------------
// GATT write queue (one write-with-response in flight at a time)
// ---------------------------------------------------------------------------

void TrumaInetX::queue_gatt_write_(uint16_t handle, std::vector<uint8_t> data, bool response) {
  if (handle == 0)
    return;
  this->gatt_queue_.push_back(GattWrite{handle, std::move(data), response});
}

void TrumaInetX::process_gatt_queue_(uint32_t now) {
  if (this->gatt_busy_ && (now - this->gatt_busy_since_) > GATT_WRITE_TIMEOUT_MS) {
    ESP_LOGW(TAG, "GATT write on 0x%04X timed out", this->gatt_busy_handle_);
    this->gatt_busy_ = false;
  }
  uint8_t budget = 4;
  while (!this->gatt_busy_ && !this->gatt_queue_.empty() && budget-- > 0) {
    if (!this->parent()->connected()) {
      this->gatt_queue_.clear();
      return;
    }
    GattWrite op = std::move(this->gatt_queue_.front());
    this->gatt_queue_.pop_front();
    esp_err_t err = esp_ble_gattc_write_char(
        this->parent()->get_gattc_if(), this->parent()->get_conn_id(), op.handle, op.data.size(), op.data.data(),
        op.response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "esp_ble_gattc_write_char(0x%04X) failed: %d", op.handle, err);
      continue;
    }
    if (op.response) {
      this->gatt_busy_ = true;
      this->gatt_busy_handle_ = op.handle;
      this->gatt_busy_since_ = now;
    }
  }
}

// ---------------------------------------------------------------------------
// Transport: 01 <len> -> 81 00 -> data -> f0 01, then async 83 xx 00 -> 03 00
// ---------------------------------------------------------------------------

void TrumaInetX::queue_frame_(std::vector<uint8_t> &&data, uint32_t delay_after) {
  if (this->tx_queue_.size() >= MAX_TX_QUEUE) {
    ESP_LOGW(TAG, "Transmit queue full, frame dropped");
    return;
  }
  OutFrame frame;
  frame.data = std::move(data);
  frame.delay_after = delay_after;
  this->tx_queue_.push_back(std::move(frame));
}

void TrumaInetX::process_tx_(uint32_t now) {
  switch (this->tx_state_) {
    case TxState::IDLE: {
      if (this->tx_queue_.empty() || (int32_t) (now - this->next_tx_at_) < 0)
        return;
      if (this->cmd_handle_ == 0 || this->data_w_handle_ == 0 || !this->parent()->connected())
        return;
      this->tx_current_ = std::move(this->tx_queue_.front());
      this->tx_queue_.pop_front();
      const auto &data = this->tx_current_.data;
      this->log_frame_("TX", data.data(), data.size());
      std::vector<uint8_t> init = {TP_INIT_DATA_TRANSFER, static_cast<uint8_t>(data.size() & 0xFF),
                                   static_cast<uint8_t>((data.size() >> 8) & 0xFF)};
      this->queue_gatt_write_(this->cmd_handle_, std::move(init), true);
      this->tx_state_ = TxState::WAIT_READY;
      this->tx_deadline_ = now + TRANSPORT_TIMEOUT_MS;
      break;
    }
    case TxState::WAIT_READY:
      if ((int32_t) (now - this->tx_deadline_) > 0) {
        ESP_LOGW(TAG, "Transport: no READY from the device, sending anyway");
        this->send_tx_data_(now);
      }
      break;
    case TxState::WAIT_ACK:
      if ((int32_t) (now - this->tx_deadline_) > 0) {
        ESP_LOGW(TAG, "Transport: no data ACK from the device");
        this->finish_tx_(now);
      }
      break;
  }
}

void TrumaInetX::send_tx_data_(uint32_t now) {
  const auto &data = this->tx_current_.data;
  size_t chunk = this->mtu_ > 3 ? this->mtu_ - 3 : 20;
  if (data.size() > chunk)
    ESP_LOGW(TAG, "Frame of %u bytes split in %u-byte writes (MTU %u)", (unsigned) data.size(), (unsigned) chunk,
             this->mtu_);
  for (size_t offset = 0; offset < data.size(); offset += chunk) {
    size_t n = std::min(chunk, data.size() - offset);
    this->queue_gatt_write_(this->data_w_handle_, std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + n),
                            false);
  }
  this->tx_state_ = TxState::WAIT_ACK;
  this->tx_deadline_ = now + TRANSPORT_TIMEOUT_MS;
}

void TrumaInetX::finish_tx_(uint32_t now) {
  uint32_t delay = std::max(this->frame_delay_, this->tx_current_.delay_after);
  this->tx_current_ = OutFrame();
  this->tx_state_ = TxState::IDLE;
  this->next_tx_at_ = now + delay;
}

void TrumaInetX::on_cmd_notify_(const uint8_t *data, uint16_t len) {
  if (this->log_frames_)
    ESP_LOGD(TAG, "RX CMD %s", hex_dump(data, len).c_str());
  if (len == 0)
    return;
  const uint32_t now = millis();
  switch (data[0]) {
    case TP_READY:
      if (this->tx_state_ == TxState::WAIT_READY) {
        if (len >= 2 && data[1] != 0x00)
          ESP_LOGW(TAG, "Transport: device not ready (status 0x%02X), sending anyway", data[1]);
        this->send_tx_data_(now);
      }
      break;
    case TP_DATA_ACK:
      if (this->tx_state_ == TxState::WAIT_ACK) {
        if (len >= 2 && data[1] != 0x01)
          ESP_LOGW(TAG, "Transport: data transfer status 0x%02X", data[1]);
        this->finish_tx_(now);
      }
      break;
    case TP_MSG_ACK:
      this->queue_gatt_write_(this->cmd_handle_, {TP_CONFIRM, 0x00}, true);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled CMD notification: %s", hex_dump(data, len).c_str());
      break;
  }
}

void TrumaInetX::on_data_notify_(const uint8_t *data, uint16_t len) {
  if (len == 0)
    return;
  if (!this->rx_buffer_.empty()) {
    // continuation of a frame larger than one notification
    this->rx_buffer_.insert(this->rx_buffer_.end(), data, data + len);
    if (this->rx_buffer_.size() >= this->rx_expected_) {
      std::vector<uint8_t> frame;
      frame.swap(this->rx_buffer_);
      this->rx_expected_ = 0;
      this->process_rx_frame_(frame.data(), frame.size());
    }
    return;
  }
  size_t expected = expected_frame_length(data, len);
  size_t full_notification = this->mtu_ > 3 ? this->mtu_ - 3 : 20;
  // Only buffer when the notification is completely full: otherwise it is a whole frame
  // even if its header announces a different size.
  if (expected > len && len >= full_notification && expected <= MAX_FRAME_SIZE) {
    this->rx_buffer_.assign(data, data + len);
    this->rx_expected_ = expected;
    this->rx_started_ = millis();
    return;
  }
  this->process_rx_frame_(data, len);
}

void TrumaInetX::process_rx_frame_(const uint8_t *data, size_t len) {
  // Acknowledge every received data frame (f0 01), as the official app does.
  this->queue_gatt_write_(this->cmd_handle_, {TP_DATA_ACK, 0x01}, true);
  this->log_frame_("RX", data, len);
  Frame frame;
  if (!parse_frame(data, len, frame)) {
    ESP_LOGW(TAG, "Short frame ignored: %s", hex_dump(data, len).c_str());
    return;
  }
  if (frame.seg_flags & SEG_IS_SEGMENTED) {
    ESP_LOGW(TAG, "Segmented frame received (flags 0x%02X, segment %u/%u) - not supported yet, please report: %s",
             frame.seg_flags, frame.seg_number, frame.seg_count, hex_dump(data, len, 48).c_str());
  }
  this->handle_frame_(frame);
}

void TrumaInetX::log_frame_(const char *direction, const uint8_t *data, size_t len) {
  if (!this->log_frames_)
    return;
  Frame frame;
  if (!parse_frame(data, len, frame)) {
    ESP_LOGD(TAG, "%s %s", direction, hex_dump(data, len).c_str());
    return;
  }
  const char *sub = frame.control == CTRL_MBP ? mbp_name(frame.sub_type) : "";
  ESP_LOGD(TAG, "%s 0x%04X -> 0x%04X %s/%s(0x%02X) corr %u: %s", direction, frame.src, frame.dest,
           control_name(frame.control), sub, frame.sub_type, frame.corr_id,
           frame.cbor.valid() ? frame.cbor.to_string(400).c_str() : "(no CBOR)");
  ESP_LOGV(TAG, "%s raw %s", direction, hex_dump(data, len, 128).c_str());
}

// ---------------------------------------------------------------------------
// iNet X session
// ---------------------------------------------------------------------------

void TrumaInetX::start_session_() {
  ESP_LOGI(TAG, "Notifications enabled, registering with the device");
  this->set_state_(SessionState::REGISTERING);
  this->state_deadline_ = millis() + REGISTRATION_TIMEOUT_MS;
  this->next_tx_at_ = millis() + 300;  // the app waits a little after connecting
  this->queue_frame_(build_registration(this->assigned_addr_));
}

void TrumaInetX::continue_initialisation_() {
  if (this->init_queued_)
    return;
  this->init_queued_ = true;
  this->set_state_(SessionState::INITIALISING);

  // 1. topic subscription, 10 topics per message
  for (size_t i = 0; i < this->topics_.size(); i += MAX_TOPICS_PER_SUBSCRIBE) {
    size_t end = std::min(this->topics_.size(), i + MAX_TOPICS_PER_SUBSCRIBE);
    std::vector<std::string> batch(this->topics_.begin() + i, this->topics_.begin() + end);
    this->queue_frame_(build_subscribe(this->assigned_addr_, batch), SUBSCRIBE_BATCH_DELAY_MS);
  }

  // 2. identity (the device remembers clients by Muid/Uuid)
  if (this->send_identity_) {
    uint16_t dest = this->resolve_destination_("MobileIdentity");
#ifdef USE_TIME
    if (this->time_ != nullptr) {
      auto now = this->time_->now();
      if (now.is_valid()) {
        this->queue_frame_(build_write(this->assigned_addr_, dest, "SystemTime", "Time",
                                       cbor::Value::make_int(static_cast<int64_t>(now.timestamp))),
                           IDENTITY_DELAY_MS);
        this->queue_frame_(build_write(this->assigned_addr_, dest, "SystemTime", "Lot", cbor::Value::make_int(0)),
                           IDENTITY_DELAY_MS);
      }
    }
#endif
    this->queue_frame_(build_write(this->assigned_addr_, dest, "MobileIdentity", "UserName",
                                   cbor::Value::make_text(this->user_name_)),
                       IDENTITY_DELAY_MS);
    this->queue_frame_(
        build_write(this->assigned_addr_, dest, "MobileIdentity", "Muid", cbor::Value::make_text(this->muid_)),
        IDENTITY_DELAY_MS);
    this->queue_frame_(
        build_write(this->assigned_addr_, dest, "MobileIdentity", "Uuid", cbor::Value::make_text(this->uuid_)),
        IDENTITY_DELAY_MS);
    this->queue_frame_(build_write_map(this->assigned_addr_, dest, "LastMessage", 1), IDENTITY_DELAY_MS);
  }

  // 3. ask the known devices for all their parameters (current values + ranges)
  for (uint16_t address : this->discovery_addresses_)
    this->queue_param_discovery_(address);
}

void TrumaInetX::queue_param_discovery_(uint16_t address) {
  if (this->probed_addresses_.count(address))
    return;
  this->probed_addresses_.insert(address);
  ESP_LOGD(TAG, "Requesting parameter discovery from 0x%04X", address);
  this->queue_frame_(build_param_discovery(this->assigned_addr_, address), DISCOVERY_DELAY_MS);
}

void TrumaInetX::refresh() {
  if (this->state_ != SessionState::READY && this->state_ != SessionState::INITIALISING) {
    ESP_LOGW(TAG, "Refresh ignored: session not ready (%s)", session_state_to_string(this->state_));
    return;
  }
  std::set<uint16_t> addresses(this->discovery_addresses_.begin(), this->discovery_addresses_.end());
  for (const auto &it : this->learned_destinations_)
    addresses.insert(it.second);
  this->probed_addresses_.clear();
  for (uint16_t address : addresses)
    this->queue_param_discovery_(address);
}

void TrumaInetX::poll_() {
  if (this->state_ != SessionState::READY || !this->tx_queue_.empty() || this->tx_state_ != TxState::IDLE)
    return;
  // Ask only the devices that actually reported parameters; fall back to the configured list.
  std::set<uint16_t> addresses;
  for (const auto &it : this->learned_destinations_)
    addresses.insert(it.second);
  if (addresses.empty())
    addresses.insert(this->discovery_addresses_.begin(), this->discovery_addresses_.end());
  ESP_LOGV(TAG, "Polling %u device(s)", (unsigned) addresses.size());
  for (uint16_t address : addresses)
    this->queue_frame_(build_param_discovery(this->assigned_addr_, address), DISCOVERY_DELAY_MS);
}

void TrumaInetX::handle_frame_(const Frame &frame) {
  switch (frame.control) {
    case CTRL_REGISTRATION: {
      const cbor::Value *addr = frame.cbor.get("addr");
      if (frame.sub_type == REG_RESPONSE && addr != nullptr && addr->is_integer()) {
        this->assigned_addr_ = static_cast<uint16_t>(addr->as_int());
        ESP_LOGI(TAG, "Registered, assigned address 0x%04X", this->assigned_addr_);
        if (this->state_ == SessionState::REGISTERING)
          this->continue_initialisation_();
      } else {
        ESP_LOGI(TAG, "Registration frame from 0x%04X (sub 0x%02X): %s", frame.src, frame.sub_type,
                 frame.cbor.to_string().c_str());
      }
      break;
    }
    case CTRL_MBP:
      switch (frame.sub_type) {
        case MBP_INFO:
          this->handle_info_(frame.src, frame.cbor);
          break;
        case MBP_PARAM_DISCOVERY_RESPONSE:
          this->handle_discovery_response_(frame.src, frame.cbor);
          break;
        case MBP_SUBSCRIBE_RESPONSE:
          ESP_LOGD(TAG, "Subscription confirmed by 0x%04X: %s", frame.src, frame.cbor.to_string(200).c_str());
          break;
        default:
          ESP_LOGI(TAG, "MBP %s(0x%02X) from 0x%04X: %s", mbp_name(frame.sub_type), frame.sub_type, frame.src,
                   frame.cbor.to_string().c_str());
          break;
      }
      break;
    default:
      ESP_LOGI(TAG, "%s frame (ctrl 0x%02X, sub 0x%02X) from 0x%04X: %s", control_name(frame.control), frame.control,
               frame.sub_type, frame.src,
               frame.cbor.valid() ? frame.cbor.to_string().c_str()
                                  : hex_dump(frame.payload.data(), frame.payload.size()).c_str());
      break;
  }
}

void TrumaInetX::handle_info_(uint16_t source, const cbor::Value &value) {
  if (value.is_array()) {
    for (const auto &item : value.items)
      this->handle_info_(source, item);
    return;
  }
  if (!value.is_map()) {
    ESP_LOGD(TAG, "INFO from 0x%04X without map payload", source);
    return;
  }
  const cbor::Value *tn = value.get("tn");
  const cbor::Value *pn = value.get("pn");
  const cbor::Value *v = value.get("v");
  if (tn != nullptr && pn != nullptr && v != nullptr && tn->is_text() && pn->is_text()) {
    this->update_param_(source, tn->str, pn->str, *v, true);
    return;
  }
  if (value.get("topics") != nullptr) {
    this->handle_discovery_response_(source, value);
    return;
  }
  ESP_LOGI(TAG, "INFO from 0x%04X with unknown layout: %s", source, value.to_string().c_str());
}

void TrumaInetX::handle_discovery_response_(uint16_t source, const cbor::Value &value) {
  const cbor::Value *topics = value.get("topics");
  if (topics == nullptr || !topics->is_array()) {
    if (value.get("tn") != nullptr) {
      // a single topic description at top level
      cbor::Value wrapper;
      wrapper.type = cbor::Type::ARRAY;
      wrapper.items.push_back(value);
      cbor::Value root;
      root.type = cbor::Type::MAP;
      root.items.push_back(cbor::Value::make_text("topics"));
      root.items.push_back(wrapper);
      this->handle_discovery_response_(source, root);
      return;
    }
    ESP_LOGI(TAG, "Parameter discovery response from 0x%04X: %s", source, value.to_string().c_str());
    return;
  }
  for (const auto &topic : topics->items) {
    const cbor::Value *tn = topic.get("tn");
    if (tn == nullptr || !tn->is_text())
      continue;
    const cbor::Value *params = topic.get("parameters");
    if (params == nullptr)
      params = topic.get("params");
    if (params == nullptr || !params->is_array()) {
      ESP_LOGD(TAG, "Topic %s from 0x%04X: %s", tn->str.c_str(), source, topic.to_string(300).c_str());
      continue;
    }
    for (const auto &p : params->items) {
      const cbor::Value *pn = p.get("pn");
      if (pn == nullptr || !pn->is_text())
        continue;
      // metadata is very useful to map a new device: log it once
      std::string meta;
      if (const auto *min = p.get("min"))
        meta += " min=" + min->to_string(32);
      if (const auto *max = p.get("max"))
        meta += " max=" + max->to_string(32);
      if (const auto *perm = p.get("perm"))
        meta += " perm=" + perm->to_string(16);
      if (const auto *avail = p.get("avail"))
        meta += " avail=" + avail->to_string(16);
      if (const auto *en = p.get("enum"))
        meta += " enum=" + en->to_string(200);
      const cbor::Value *v = p.get("v");
      const bool known = this->params_.count(tn->str + "." + pn->str) > 0;
      if (!known) {
        ESP_LOGI(TAG, "[0x%04X] %s.%s = %s%s", source, tn->str.c_str(), pn->str.c_str(),
                 v != nullptr ? v->to_string(64).c_str() : "?", meta.c_str());
      } else {
        ESP_LOGV(TAG, "[0x%04X] %s.%s = %s%s", source, tn->str.c_str(), pn->str.c_str(),
                 v != nullptr ? v->to_string(64).c_str() : "?", meta.c_str());
      }
      if (v != nullptr)
        this->update_param_(source, tn->str, pn->str, *v, true, "poll");
    }
  }
}

void TrumaInetX::update_param_(uint16_t source, const std::string &topic, const std::string &param,
                               const cbor::Value &value, bool from_device, const char *via) {
  if (from_device && source != ADDR_MESSAGE_BROKER && source != ADDR_BROADCAST && source != this->assigned_addr_) {
    this->learned_destinations_[topic] = source;
    if (this->auto_discovery_ && !this->probed_addresses_.count(source) &&
        (this->state_ == SessionState::READY || this->state_ == SessionState::INITIALISING)) {
      ESP_LOGI(TAG, "New device address 0x%04X seen, requesting its parameters", source);
      this->queue_param_discovery_(source);
    }
  }

  const std::string key = topic + "." + param;
  auto it = this->params_.find(key);
  if (it == this->params_.end()) {
    ESP_LOGI(TAG, "New parameter %s = %s (from 0x%04X)", key.c_str(), value.to_string(64).c_str(), source);
    this->params_[key] = ParamEntry{value, source};
  } else {
    std::string old_str = it->second.value.to_string(64);
    std::string new_str = value.to_string(64);
    if (old_str != new_str) {
      if (from_device) {
        ESP_LOGI(TAG, "%s: %s -> %s (from 0x%04X, %s)", key.c_str(), old_str.c_str(), new_str.c_str(), source, via);
      } else {
        ESP_LOGD(TAG, "%s: %s -> %s (sent)", key.c_str(), old_str.c_str(), new_str.c_str());
      }
    }
    it->second.value = value;
    if (from_device)
      it->second.source = source;
  }

  for (auto &listener : this->listeners_) {
    if (listener.topic == topic && listener.param == param)
      listener.callback(value);
  }
}

uint16_t TrumaInetX::resolve_destination_(const std::string &topic) const {
  auto configured = this->destinations_.find(topic);
  if (configured != this->destinations_.end())
    return configured->second;
  auto learned = this->learned_destinations_.find(topic);
  if (learned != this->learned_destinations_.end())
    return learned->second;
  return this->default_destination_;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool TrumaInetX::write(const std::string &topic, const std::string &param, const cbor::Value &value,
                       uint16_t destination) {
  if (this->state_ != SessionState::READY && this->state_ != SessionState::INITIALISING &&
      this->state_ != SessionState::REGISTERING) {
    ESP_LOGW(TAG, "Write %s.%s ignored: not connected (%s)", topic.c_str(), param.c_str(),
             session_state_to_string(this->state_));
    return false;
  }
  uint16_t dest = destination != 0 ? destination : this->resolve_destination_(topic);
  ESP_LOGI(TAG, "Write %s.%s = %s -> 0x%04X", topic.c_str(), param.c_str(), value.to_string(64).c_str(), dest);
  this->queue_frame_(build_write(this->assigned_addr_, dest, topic, param, value));
  if (this->optimistic_)
    this->update_param_(this->assigned_addr_, topic, param, value, false);
  return true;
}

void TrumaInetX::dump_parameters() {
  ESP_LOGI(TAG, "%u known parameters (state %s, address 0x%04X):", (unsigned) this->params_.size(),
           session_state_to_string(this->state_), this->assigned_addr_);
  for (const auto &it : this->params_) {
    ESP_LOGI(TAG, "  [0x%04X] %s = %s", it.second.source, it.first.c_str(), it.second.value.to_string(96).c_str());
  }
  for (const auto &it : this->learned_destinations_)
    ESP_LOGI(TAG, "  route: %s -> 0x%04X", it.first.c_str(), it.second);
}

const cbor::Value *TrumaInetX::get_value(const std::string &topic, const std::string &param) const {
  auto it = this->params_.find(topic + "." + param);
  if (it == this->params_.end())
    return nullptr;
  return &it->second.value;
}

void TrumaInetX::register_listener(const std::string &topic, const std::string &param,
                                   std::function<void(const cbor::Value &)> &&callback) {
  this->listeners_.push_back(Listener{topic, param, std::move(callback)});
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
