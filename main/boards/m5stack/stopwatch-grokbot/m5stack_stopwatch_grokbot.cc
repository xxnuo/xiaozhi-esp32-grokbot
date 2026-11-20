#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include <atomic>
#include "M5IOE1.h"
#include "M5PM1.h"
#include "application.h"
#include "assets/lang_config.h"
#include "backlight.h"
#include "bmi270_api.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/grokbot_display.h"
#include "esp_lcd_co5300.h"
#include "grokbot_inputs.h"
#include "grokbot_rotation.h"
#include "mcp_server.h"
#include "settings.h"
#include "wifi_board.h"

#define TAG "M5StackStopwatchGrokBot"
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

// CO5300 AMOLED: initialize at full brightness, then restore the saved setting.
static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // {cmd, { data }, data_size, delay_ms}
    {0xFE, (uint8_t[]){0x00}, 0, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 10},  // RGB565
    // Keep scanning ahead of the writer: a 360-row update takes longer than
    // scanning those rows. Line 128 leaves about 2.7ms at the top and 27ms
    // until the next scan reaches the image bottom.
    {0x35, (uint8_t[]){0x00}, 1, 10},  // Vertical TE only, positive pulse
    {0x44, (uint8_t[]){0x00, GrokbotDisplay::kTeScanline}, 2, 10},  // Tear line = 128
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},  // Column address: 0-465
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},  // Row address: 0-465
    {0x11, (uint8_t[]){0x00}, 0, 120},                  // Exit sleep
    {0x29, (uint8_t[]){0x00}, 0, 20},                   // Display on
};

class StopwatchBacklight : public Backlight {
public:
    StopwatchBacklight(esp_lcd_panel_io_handle_t panel_io, Display* display)
        : panel_io_(panel_io), display_(display) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        DisplayLockGuard lock(display_);
        uint8_t data[] = {static_cast<uint8_t>((255 * brightness) / 100)};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;

        esp_err_t err = esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, data, sizeof(data));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set display brightness: %s", esp_err_to_name(err));
        }
    }

private:
    esp_lcd_panel_io_handle_t panel_io_;
    Display* display_;
};

class M5StackStopwatchGrokbotBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    M5PM1 pmic_;
    bool pmic_ready_ = false;
    M5IOE1 ioe_;
    Button button1_;
    Button button2_;
    GrokbotDisplay* display_;
    StopwatchBacklight* backlight_;
    i2c_master_dev_handle_t touch_ = nullptr;
    bmi270_handle_t imu_ = nullptr;
    int boot_orientation_ = 0;
    std::atomic<bool> interaction_pending_{false};
    int saved_volume_ = 70;
    esp_timer_handle_t countdown_timer_ = nullptr;
    esp_timer_handle_t motor_off_timer_ = nullptr;
    int countdown_remaining_sec_ = 0;
    std::string countdown_label_ = "Timer";

    void QueueInteraction(GrokbotInputs::Reaction reaction, int64_t now_ms) {
        if (reaction == GrokbotInputs::Reaction::kNone ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
            interaction_pending_.exchange(true)) {
            return;
        }
        const char* emotion = "curious";
        switch (reaction) {
            case GrokbotInputs::Reaction::kTap:
                emotion = "happy";
                break;
            case GrokbotInputs::Reaction::kStroke:
                emotion = "loving";
                break;
            case GrokbotInputs::Reaction::kShake:
                emotion = "confused";
                break;
            default:
                break;
        }
        Application::GetInstance().Schedule([this, emotion, now_ms]() {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
                esp_timer_get_time() / 1000 - now_ms < 400) {
                if (display_->OnInteraction(emotion))
                    ESP_LOGI(TAG, "Local interaction accepted: %s", emotion);
            }
            interaction_pending_.store(false);
        });
    }

    bool ConfigurePowerButton() {
        if (!pmic_ready_)
            return false;
        bool was_pressed = false;
        if (pmic_.btnGetFlag(&was_pressed) != M5PM1_OK ||
            pmic_.setSingleResetDisable(true) != M5PM1_OK) {
            ESP_LOGW(TAG, "Could not configure power button events");
            return false;
        }
        if (pmic_.setDoubleOffDisable(true) != M5PM1_OK) {
            pmic_.setSingleResetDisable(false);
            ESP_LOGW(TAG, "Could not defer hardware shutdown");
            return false;
        }
        ESP_LOGI(TAG, "Power button shutdown notice enabled");
        return true;
    }

    void InputTask() {
        GrokbotInputs inputs;
        const bool power_button = ConfigurePowerButton();
        int64_t shutdown_at_ms = 0;
        bool power_press_pending = false;
        unsigned imu_failures = 0;
        bool touch_seen = false;
        bool imu_sample_seen = false;
        bool have_charge_state = false;
        bool was_charging = false;
        unsigned charge_poll_div = 0;
        while (true) {
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (power_button) {
                if (shutdown_at_ms && now_ms >= shutdown_at_ms) {
                    if (pmic_.shutdown() != M5PM1_OK) {
                        ESP_LOGW(TAG, "Power button shutdown request failed");
                        display_->ShowNotification(display_->IsChinese() ? "关机失败" : "Shutdown failed");
                    }
                    shutdown_at_ms = 0;
                } else if (!shutdown_at_ms) {
                    bool pressed = false;
                    bool was_pressed = false;
                    // Read the latched press even if it started and ended between polls.
                    if (pmic_.btnGetState(&pressed) == M5PM1_OK &&
                        pmic_.btnGetFlag(&was_pressed) == M5PM1_OK) {
                        power_press_pending |= pressed || was_pressed;
                    } else {
                        pressed = true;
                    }
                    if (power_press_pending && !pressed) {
                        power_press_pending = false;
                        ESP_LOGI(TAG, "Power button released; showing shutdown notice");
                        display_->ShowShutdownNotice();
                        shutdown_at_ms = esp_timer_get_time() / 1000 + 1000;
                    }
                }
            }
            bool touch_active = false;
            float look_x = 0.0f;
            float look_y = 0.0f;
            if (touch_) {
                // CST820B may NACK while asleep, just like M5GFX's CST816S driver.
                // Probe first so an absent/sleeping controller does not flood I2C error logs.
                const auto probe = i2c_master_probe(i2c_bus_, 0x15, 10);
                if (probe == ESP_OK) {
                    const uint8_t reg = 0x02;
                    uint8_t data[5] = {};
                    if (i2c_master_transmit_receive(touch_, &reg, 1, data, sizeof(data), 20) ==
                        ESP_OK) {
                        int x = ((data[1] & 0x0f) << 8) | data[2];
                        int y = ((data[3] & 0x0f) << 8) | data[4];
                        if (data[0] <= 1 &&
                            (!data[0] || (x < DISPLAY_WIDTH && y < DISPLAY_HEIGHT))) {
                            if (!touch_seen) {
                                ESP_LOGI(TAG, "CST820B responding at 0x15");
                                touch_seen = true;
                            }
                            if (data[0] == 1) {
                                MapBootTouch(boot_orientation_, DISPLAY_WIDTH, x, y);
                                if (display_ && display_->HandleTouch(true, x, y, now_ms)) {
                                    inputs.CancelTouch();
                                } else {
                                    touch_active = true;
                                    look_x = (x - DISPLAY_WIDTH * 0.5f) / (DISPLAY_WIDTH * 0.5f);
                                    look_y = -(y - DISPLAY_HEIGHT * 0.5f) / (DISPLAY_HEIGHT * 0.5f);
                                    QueueInteraction(inputs.Touch(true, x, y, now_ms), now_ms);
                                }
                            } else {
                                if (display_ && display_->HandleTouch(false, x, y, now_ms)) {
                                    inputs.CancelTouch();
                                } else {
                                    QueueInteraction(inputs.Touch(false, x, y, now_ms), now_ms);
                                }
                            }
                        } else {
                            inputs.CancelTouch();
                        }
                    } else {
                        inputs.CancelTouch();
                    }
                } else if (probe == ESP_ERR_NOT_FOUND) {
                    if (display_ && display_->HandleTouch(false, 0, 0, now_ms)) {
                        inputs.CancelTouch();
                    } else {
                        QueueInteraction(inputs.Touch(false, 0, 0, now_ms), now_ms);
                    }
                } else {
                    inputs.CancelTouch();
                }
            }
            if (imu_) {
                struct bmi2_sens_data sample = {};
                if (bmi2_get_sensor_data(&sample, imu_) == BMI2_OK) {
                    imu_failures = 0;
                    const int x_mg = int(sample.acc.x) * 1000 / 8192;
                    const int y_mg = int(sample.acc.y) * 1000 / 8192;
                    const int z_mg = int(sample.acc.z) * 1000 / 8192;
                    const int64_t norm =
                        int64_t(x_mg) * x_mg + int64_t(y_mg) * y_mg + int64_t(z_mg) * z_mg;
                    if (!imu_sample_seen && norm >= 800 * 800 && norm <= 1200 * 1200) {
                        ESP_LOGI(TAG, "BMI270 first valid acceleration: x=%d y=%d z=%d mg", x_mg,
                                 y_mg, z_mg);
                        imu_sample_seen = true;
                    }
                    if (!touch_active && norm >= 800 * 800 && norm <= 1200 * 1200) {
                        auto axis_tilt = [](int mg) -> float {
                            const int abs_mg = std::abs(mg);
                            if (abs_mg <= 120)
                                return 0.0f;
                            const float mag =
                                std::min(1.0f, static_cast<float>(abs_mg - 120) / 430.0f);
                            return mg >= 0 ? mag : -mag;
                        };
                        look_x = axis_tilt(x_mg);
                        look_y = -axis_tilt(y_mg);
                    }
                    QueueInteraction(inputs.Motion(x_mg, y_mg, z_mg, now_ms), now_ms);
                } else if (++imu_failures >= 5) {
                    ESP_LOGW(TAG,
                             "BMI270 unavailable after repeated read errors; disabling motion");
                    bmi270_sensor_del(&imu_);
                }
            }
            if (display_) {
                display_->SetLookTarget(look_x, look_y);
            }
            if (++charge_poll_div >= 20) {
                charge_poll_div = 0;
                int level = 0;
                bool charging = false;
                bool discharging = false;
                if (GetBatteryLevel(level, charging, discharging)) {
                    if (have_charge_state && !was_charging && charging &&
                        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                        Application::GetInstance().Schedule([this]() {
                            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                                display_->OnInteraction("happy");
                            }
                        });
                    }
                    was_charging = charging;
                    have_charge_state = true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeInputs() {
        m5ioe1_err_t touch_reset = M5IOE1_OK;
        ioe_.pinModeWithRes(IOE_PIN_TOUCH_RST, OUTPUT, &touch_reset);
        if (touch_reset == M5IOE1_OK)
            touch_reset = ioe_.setDriveMode(IOE_PIN_TOUCH_RST, M5IOE1_DRIVE_PUSHPULL);
        if (touch_reset == M5IOE1_OK)
            ioe_.digitalWriteWithRes(IOE_PIN_TOUCH_RST, LOW, &touch_reset);
        if (touch_reset == M5IOE1_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            ioe_.digitalWriteWithRes(IOE_PIN_TOUCH_RST, HIGH, &touch_reset);
            if (touch_reset == M5IOE1_OK) {
                vTaskDelay(pdMS_TO_TICKS(50));
                i2c_device_config_t config = {};
                config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
                config.device_address = 0x15;
                config.scl_speed_hz = 100000;
                if (i2c_master_bus_add_device(i2c_bus_, &config, &touch_) != ESP_OK)
                    ESP_LOGW(TAG, "Touch device registration failed; disabling touch");
            }
        }
        if (touch_reset != M5IOE1_OK) {
            ESP_LOGW(TAG, "Touch reset unavailable; disabling touch");
        }

        if (i2c_master_probe(i2c_bus_, BMI270_I2C_ADDRESS, 20) == ESP_OK) {
            i2c_config_t bus_config = {};
            bus_config.mode = I2C_MODE_MASTER;
            bus_config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
            bus_config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
            bus_config.sda_pullup_en = true;
            bus_config.scl_pullup_en = true;
            bus_config.master.clk_speed = 100000;
            // i2c_bus v2 attaches to the existing master bus instead of replacing it.
            auto shared_bus = i2c_bus_create(I2C_NUM_0, &bus_config);
            const auto result =
                shared_bus
                    ? bmi270_sensor_create(shared_bus, &imu_, bmi270_config_file,
                                           BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE)
                    : ESP_FAIL;
            if (result == ESP_OK && imu_) {
                struct bmi2_sens_config config = {};
                config.type = BMI2_ACCEL;
                config.cfg.acc.odr = BMI2_ACC_ODR_25HZ;
                config.cfg.acc.range = BMI2_ACC_RANGE_4G;
                config.cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
                config.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
                const uint8_t sensors[] = {BMI2_ACCEL};
                if (bmi270_set_sensor_config(&config, 1, imu_) != BMI2_OK ||
                    bmi270_sensor_enable(sensors, 1, imu_) != BMI2_OK) {
                    ESP_LOGW(TAG, "BMI270 accelerometer configuration failed");
                    bmi270_sensor_del(&imu_);
                }
            } else {
                ESP_LOGW(TAG, "BMI270 initialization failed: %s", esp_err_to_name(result));
                if (imu_)
                    bmi270_sensor_del(&imu_);
            }
        }
        ESP_LOGI(TAG, "Optional inputs: touch=%s, BMI270=%s",
                 touch_ ? "polling (presence unconfirmed)" : "disabled",
                 imu_ ? "ready" : "disabled");
    }

    void InitializeInputTask() {
        if ((touch_ || imu_ || pmic_ready_) &&
            xTaskCreate(
                [](void* arg) { static_cast<M5StackStopwatchGrokbotBoard*>(arg)->InputTask(); },
                "grokbot_inputs", 4096, this, 1, nullptr) != pdPASS) {
            ESP_LOGW(TAG, "Failed to start input polling task");
        }
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        if (ioe_.begin(i2c_bus_, M5IOE1_I2C_ADDR, M5IOE1_I2C_FREQ_100K, M5IOE1_INT_MODE_POLLING) !=
            M5IOE1_OK) {
            ESP_LOGE(TAG, "M5IOE1 begin failed");
            return;
        }

        if (pmic_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) != M5PM1_OK) {
            ESP_LOGE(TAG, "M5PM1 begin failed");
            return;
        }

        pmic_ready_ = true;
        pmic_.setChargeEnable(true);
        pmic_.setBoostEnable(true);
        pmic_.pinMode(PMIC_PIN_CHARGE_STATE, INPUT);
        pmic_.pinMode(PMIC_PIN_CHARGE_PROG, OUTPUT);
        pmic_.digitalWrite(PMIC_PIN_CHARGE_PROG, LOW);  // Set charge current to 425mA

        ioe_.pinMode(IOE_PIN_LCD_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_LCD_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);

        ioe_.pinMode(IOE_PIN_CODEC_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_CODEC_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_CODEC_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_PA_EN, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_PA_EN, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_PA_EN, HIGH);

        ioe_.pinMode(IOE_PIN_MOTOR, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_MOTOR, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_MOTOR, LOW);

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_QSPI_SCK;
        buscfg.data0_io_num = DISPLAY_QSPI_D0;
        buscfg.data1_io_num = DISPLAY_QSPI_D1;
        buscfg.data2_io_num = DISPLAY_QSPI_D2;
        buscfg.data3_io_num = DISPLAY_QSPI_D3;
        // LVGL renders into an internal 20-line DMA strip. Keeping the bus transfer
        // limit at that size avoids a large bounce buffer while preserving enough
        // headroom for the display driver.
        buscfg.max_transfer_sz = DISPLAY_WIDTH * 80 * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void ReadBootOrientation() {
        if (!imu_ || !Settings("display").GetBool("boot_orient"))
            return;
        // Require two matching fresh readings at the 25 Hz sensor rate.
        int previous = -1;
        for (int attempt = 0; attempt < 10; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(50));
            bmi2_sens_data sample = {};
            if (bmi2_get_sensor_data(&sample, imu_) != BMI2_OK ||
                !(sample.status & BMI2_DRDY_ACC)) {
                previous = -1;
                continue;
            }
            const int orientation = BootOrientation(sample.acc.x, sample.acc.y, sample.acc.z);
            if (orientation >= 0 && orientation == previous) {
                boot_orientation_ = orientation;
                ESP_LOGI(TAG, "Boot orientation: %d degrees (x=%d y=%d z=%d raw)",
                         orientation * 90, sample.acc.x, sample.acc.y, sample.acc.z);
                return;
            }
            previous = orientation;
        }
        ESP_LOGW(TAG, "Boot orientation: no stable tilted sample; using default");
    }

    void InitializeDisplay() {
        ioe_.digitalWrite(IOE_PIN_LCD_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(120));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_QSPI_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        // The M5Stack StopWatch CO5300 wiring is validated at 80 MHz by the
        // standalone GrokBot firmware. Keep that rate to reduce full-frame latency.
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_QSPI_HOST, &io_config, &panel_io));

        co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {.use_qspi_interface = 1},
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));

        const bool swap = boot_orientation_ % 2 != 0;
        const bool mirror_x = boot_orientation_ == 1 || boot_orientation_ == 2;
        const bool mirror_y = boot_orientation_ >= 2;
        // The 466px visible area sits in 480px GRAM. Rotated rows need the
        // 14px origin used by the StopWatch panel mapping to cover the edge.
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, swap ? 0 : 6, swap ? 14 : 0));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, swap));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, mirror_x, mirror_y));
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new GrokbotDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, mirror_x,
                                      mirror_y, swap, DISPLAY_TE_PIN);
        display_->SetSettingsCallbacks(
            []() {
                std::vector<std::string> names;
                for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
                    names.push_back(item.ssid);
                }
                return names;
            },
            [this](int index) {
                auto& ssid_mgr = SsidManager::GetInstance();
                const auto list = ssid_mgr.GetSsidList();
                if (index < 0 || index >= static_cast<int>(list.size()))
                    return;
                const std::string target_ssid = list[index].ssid;
                ssid_mgr.SetDefaultSsid(index);
                const bool zh = display_->IsChinese();
                display_->ShowNotification((zh ? "正在连接: " : "Connecting: ") + target_ssid,
                                           3000);
                xTaskCreate(
                    [](void*) {
                        auto& wifi = WifiManager::GetInstance();
                        if (wifi.IsConfigMode()) {
                            wifi.StopConfigAp();
                        } else {
                            wifi.StopStation();
                        }
                        vTaskDelay(pdMS_TO_TICKS(150));
                        wifi.StartStation();
                        vTaskDelete(nullptr);
                    },
                    "wifi_switch", 4096, nullptr, 2, nullptr);
            },
            [this]() {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                    display_->ShowNotification(
                        display_->IsChinese() ? "已在配网模式" : "Already in Wi-Fi Config", 2500);
                    return;
                }
                if (app.GetDeviceState() == kDeviceStateActivating) {
                    app.SetDeviceState(kDeviceStateIdle);
                }
                EnterWifiConfigMode();
            });
        backlight_ = new StopwatchBacklight(panel_io, display_);
        backlight_->RestoreBrightness();
    }

    void InitializeButtons() {
        // Button1: wake / toggle conversation
        button1_.OnClick([this]() {
            if (display_ && display_->IsSettingsMenuOpen()) {
                display_->SettingsSelectItem();
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting &&
                !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Button2: volume 0 -> 10 -> ... -> 100 -> 0
        button2_.OnClick([this]() {
            if (display_ && display_->IsSettingsMenuOpen()) {
                display_->SettingsNextItem();
                return;
            }
            auto* codec = GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ":" +
                                           std::to_string(volume) + "%");
        });

        // Button2 double-click: cycle screen brightness 25% -> 50% -> 75% -> 100%
        button2_.OnDoubleClick([this]() {
            if (!backlight_)
                return;
            const uint8_t current = backlight_->brightness();
            const uint8_t next = current < 35 ? 50 : current < 65 ? 75 : current < 90 ? 100 : 25;
            backlight_->SetBrightness(next, true);
            if (display_) {
                display_->ShowNotification("Brightness: " + std::to_string(next) + "%");
            }
        });

        // Button2 long-press: quick mute / unmute
        button2_.OnLongPress([this]() {
            auto* codec = GetAudioCodec();
            const int current = codec->output_volume();
            int target = 0;
            if (current > 0) {
                saved_volume_ = current;
                target = 0;
            } else {
                target = saved_volume_ > 0 ? saved_volume_ : 70;
            }
            codec->SetOutputVolume(target);
            if (display_) {
                display_->ShowNotification(target == 0 ? "Muted"
                                                       : std::string(Lang::Strings::VOLUME) + ":" +
                                                             std::to_string(target) + "%");
            }
        });
    }

    void InitializeTimerTool() {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<M5StackStopwatchGrokbotBoard*>(arg);
                    Application::GetInstance().Schedule([self]() { self->OnCountdownTick(); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stopwatch_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &countdown_timer_));

        esp_timer_create_args_t motor_args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<M5StackStopwatchGrokbotBoard*>(arg);
                    self->ioe_.digitalWrite(IOE_PIN_MOTOR, LOW);
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "motor_off",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&motor_args, &motor_off_timer_));

        McpServer::GetInstance().AddTool(
            "self.stopwatch.timer",
            "Start or cancel a countdown / Pomodoro timer on the stopwatch. "
            "Set seconds > 0 to start, or seconds = 0 to cancel.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 0, 7200),
                          Property("label", kPropertyTypeString, std::string("Timer"))}),
            [this](const PropertyList& properties) -> ReturnValue {
                const int seconds = properties["seconds"].value<int>();
                const std::string label = properties["label"].value<std::string>();
                esp_timer_stop(countdown_timer_);
                countdown_remaining_sec_ = seconds;
                countdown_label_ = label.empty() ? "Timer" : label;
                if (seconds <= 0) {
                    if (display_)
                        display_->ShowNotification("Timer canceled", 2000);
                    return std::string("Timer canceled");
                }
                esp_timer_start_periodic(countdown_timer_, 1000000);
                UpdateCountdownBanner();
                return "Started " + countdown_label_ + " for " + std::to_string(seconds) + "s";
            });
    }

    void UpdateCountdownBanner() {
        if (!display_)
            return;
        const int mins = countdown_remaining_sec_ / 60;
        const int secs = countdown_remaining_sec_ % 60;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s %02d:%02d", countdown_label_.c_str(), mins, secs);
        display_->ShowNotification(buf, 1200);
    }

    void OnCountdownTick() {
        if (countdown_remaining_sec_ <= 0) {
            esp_timer_stop(countdown_timer_);
            return;
        }
        --countdown_remaining_sec_;
        if (countdown_remaining_sec_ > 0) {
            UpdateCountdownBanner();
            return;
        }
        esp_timer_stop(countdown_timer_);
        if (display_) {
            display_->ShowNotification(countdown_label_ + " Done!", 5000);
            display_->SetEmotion("notifying");
        }
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
        ioe_.digitalWrite(IOE_PIN_MOTOR, HIGH);
        esp_timer_start_once(motor_off_timer_, 180000);
    }

public:
    M5StackStopwatchGrokbotBoard()
        : i2c_bus_(nullptr),
          button1_(BUTTON1_GPIO),
          button2_(BUTTON2_GPIO),
          display_(nullptr),
          backlight_(nullptr) {
        InitializeI2c();
        InitializeInputs();
        ReadBootOrientation();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        InitializeInputTask();
        InitializeTimerTool();
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_GPIO_PA, AUDIO_CODEC_ES8311_ADDR, false);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override { return backlight_; }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        uint16_t voltage_mv = 0;
        if (pmic_.readVbat(&voltage_mv) != M5PM1_OK) {
            return false;
        }

        int charge_state_level = pmic_.digitalRead(PMIC_PIN_CHARGE_STATE);
        if (charge_state_level < 0) {
            // Charge status read failed; leave charging/discharging unknown
            charging = false;
            discharging = false;
        } else {
            // M5PM1 charge status is active low: 0 means charging, 1 means not charging
            charging = (charge_state_level == 0);
            discharging = !charging;
        }

        const int BATTERY_MIN_VOLTAGE = 3400;
        const int BATTERY_MAX_VOLTAGE = 4200;
        if (voltage_mv < BATTERY_MIN_VOLTAGE) {
            level = 0;
        } else if (voltage_mv > BATTERY_MAX_VOLTAGE) {
            level = 100;
        } else {
            level = ((voltage_mv - BATTERY_MIN_VOLTAGE) * 100) /
                    (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
        }
        return true;
    }
};

DECLARE_BOARD(M5StackStopwatchGrokbotBoard);
