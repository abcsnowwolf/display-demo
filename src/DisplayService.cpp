#include "DisplayService.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <time.h>

#include <vector>

#include "board/lcd.h"
#include "board/xl9555.h"
#include "gfx/GfxAlloc.h"

DisplayService display;

namespace {

constexpr int WIDTH = SceneBuilder::WIDTH;
constexpr int HEIGHT = SceneBuilder::HEIGHT;
constexpr uint32_t TASK_STACK = 16384;
constexpr size_t MAX_SAVED_BYTES = 6 * 1024;  // nvs is 20 KiB, shared with WiFi
constexpr const char* PREFS_NS = "display";
constexpr const char* PREFS_KEY = "screen";
constexpr size_t MAX_CANVAS_ELEMENTS = 200;
constexpr uint32_t FLASH_SAVE_DELAY_MS = 3000;

bool due(uint32_t at, uint32_t now) {
    return at && int32_t(now - at) >= 0;
}

uint32_t readU32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

// Network-related fields only; the clock is maintained by the display task.
bool sameNetwork(const SystemInfo& a, const SystemInfo& b) {
    return a.deviceName == b.deviceName && a.bleName == b.bleName && a.wifi == b.wifi && a.ssid == b.ssid &&
           a.mcpUrl == b.mcpUrl;
}

}  // namespace

bool DisplayService::begin() {
    lock_ = xSemaphoreCreateMutex();

    xl9555_init();
    lcd_hw_init();
    lcd.fillScreen(TFT_BLACK);

    // A whole frame fits only in PSRAM; without it render in 40-row strips.
    status_.fullFrame = gfxHasPsram();
    if (status_.fullFrame) {
        bandHeight_ = HEIGHT;
        frame_ = static_cast<uint16_t*>(heap_caps_malloc(WIDTH * HEIGHT * 2, MALLOC_CAP_SPIRAM));
    }
    if (!frame_) {
        status_.fullFrame = false;
        bandHeight_ = 40;
        frame_ = static_cast<uint16_t*>(heap_caps_malloc(WIDTH * bandHeight_ * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    if (!frame_) {
        Serial.println("[Display] out of memory for the frame buffer");
        return false;
    }
    fonts_.setCacheBudget(status_.fullFrame ? 1536 * 1024 : 40 * 1024);

    fontsReady_ = mapFonts();
    status_.fontsReady = fontsReady_;
    Serial.printf("[Display] fonts %s, %s rendering\n", fontsReady_ ? "ready" : "MISSING",
                  status_.fullFrame ? "full-frame" : "strip");
    if (!fontsReady_) showFontError();

    loadSaved();
    BaseType_t ok = xTaskCreatePinnedToCore(taskEntry, "display", TASK_STACK, this, 1, &task_, 1);
    return ok == pdPASS;
}

bool DisplayService::mapFonts() {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fonts");
    if (!part) {
        Serial.println("[Display] no 'fonts' partition in the partition table");
        return false;
    }
    uint8_t head[8];
    if (esp_partition_read(part, 0, head, sizeof(head)) != ESP_OK || memcmp(head, "FPK1", 4) != 0) {
        Serial.println("[Display] font partition is empty; run `pio run -t uploadfonts`");
        return false;
    }
    uint32_t count = readU32(head + 4);
    if (count == 0 || count > 32) return false;
    std::vector<uint8_t> entries(count * 32);
    if (esp_partition_read(part, 8, entries.data(), entries.size()) != ESP_OK) return false;
    uint32_t end = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = entries.data() + i * 32;
        end = std::max(end, readU32(e + 24) + readU32(e + 28));
    }
    if (end > part->size) return false;

    size_t mapSize = std::min<size_t>((end + 0xFFFF) & ~0xFFFFu, part->size);
    const void* ptr = nullptr;
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, mapSize, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
#else
    spi_flash_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, mapSize, SPI_FLASH_MMAP_DATA, &ptr, &handle);
#endif
    if (err != ESP_OK) {
        Serial.printf("[Display] mmap of %u bytes failed: %s\n", unsigned(mapSize), esp_err_to_name(err));
        return false;
    }
    return fonts_.begin(static_cast<const uint8_t*>(ptr), end);
}

void DisplayService::showFontError() {
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_ORANGE);
    lcd.setFont(&fonts::Font4);
    lcd.drawString("Fonts not installed", 16, 60);
    lcd.setTextColor(TFT_WHITE);
    lcd.setFont(&fonts::Font2);
    lcd.drawString("Flash the font partition once:", 16, 110);
    lcd.setTextColor(TFT_CYAN);
    lcd.drawString("pio run -t uploadfonts", 16, 136);
}

// ------------------------------------------------------------------ API side

DisplayService::Result DisplayService::submit(std::shared_ptr<Request> req, uint32_t timeoutMs) {
    if (!task_) {
        Result r;
        r.error = "display is not running";
        return r;
    }
    req->done = xSemaphoreCreateBinary();
    xSemaphoreTake(lock_, portMAX_DELAY);
    queue_.push_back(req);
    xSemaphoreGive(lock_);
    xTaskNotifyGive(task_);
    // The request is shared, so a timed-out caller can leave safely.
    if (xSemaphoreTake(req->done, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        Result r;
        r.ok = true;
        r.report.warnings.push_back("still rendering; the screen will update shortly");
        return r;
    }
    return req->result;
}

DisplayService::Result DisplayService::show(JsonDocument spec, uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::Show;
    req->spec = std::move(spec);
    return submit(req, timeoutMs);
}

DisplayService::Result DisplayService::toast(JsonDocument spec, uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::Toast;
    req->spec = std::move(spec);
    return submit(req, timeoutMs);
}

DisplayService::Result DisplayService::clear(uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::Clear;
    return submit(req, timeoutMs);
}

DisplayService::Result DisplayService::canvasDraw(JsonDocument args, uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::CanvasDraw;
    req->spec = std::move(args);
    return submit(req, timeoutMs);
}

DisplayService::Result DisplayService::canvasUpdate(JsonDocument args, uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::CanvasUpdate;
    req->spec = std::move(args);
    return submit(req, timeoutMs);
}

DisplayService::Result DisplayService::canvasRemove(JsonDocument args, uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::CanvasRemove;
    req->spec = std::move(args);
    return submit(req, timeoutMs);
}

DisplayService::Result DisplayService::canvasGet(uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::CanvasGet;
    return submit(req, timeoutMs);
}

DisplayService::Result DisplayService::setBacklight(bool on, uint32_t timeoutMs) {
    auto req = std::make_shared<Request>();
    req->kind = Kind::Backlight;
    req->flag = on;
    return submit(req, timeoutMs);
}

void DisplayService::updateSystem(const SystemInfo& info) {
    if (!lock_) return;
    bool changed = false;
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (!sameNetwork(info, pendingSys_)) {
        pendingSys_ = info;
        sysDirty_ = changed = true;
    }
    xSemaphoreGive(lock_);
    if (changed && task_) xTaskNotifyGive(task_);
}

DisplayService::Status DisplayService::status() {
    xSemaphoreTake(lock_, portMAX_DELAY);
    Status s = status_;
    xSemaphoreGive(lock_);
    return s;
}

// --------------------------------------------------------------- task side

void DisplayService::taskEntry(void* arg) {
    static_cast<DisplayService*>(arg)->run();
}

void DisplayService::run() {
    selectSystemScreen();
    for (;;) {
        std::deque<std::shared_ptr<Request>> work;
        bool sysChanged = false;
        xSemaphoreTake(lock_, portMAX_DELAY);
        work.swap(queue_);
        if (sysDirty_) {
            SystemInfo s = pendingSys_;
            sysDirty_ = false;
            sysChanged = !sameNetwork(s, sys_);
            sys_.deviceName = s.deviceName;
            sys_.bleName = s.bleName;
            sys_.wifi = s.wifi;
            sys_.ssid = s.ssid;
            sys_.mcpUrl = s.mcpUrl;
        }
        xSemaphoreGive(lock_);

        if (sysChanged) {
            // Restore the last persisted screen the first time we are online.
            if (systemScreen_ && !saved_.isNull() && sys_.wifi == SystemInfo::Wifi::Connected) {
                screen_ = saved_;
                systemScreen_ = false;
                slide_ = page_ = 0;
                expireAt_ = 0;
            }
            if (systemScreen_) selectSystemScreen();
            dirty_ = dirty_ || systemScreen_ || lastReport_.usesClock;
        }

        for (auto& req : work) {
            handle(*req);
            xSemaphoreGive(req->done);
        }

        uint32_t now = millis();
        if (due(expireAt_, now)) {
            expireAt_ = 0;
            if (!saved_.isNull()) {
                screen_ = saved_;
                systemScreen_ = false;
            } else {
                screen_.clear();
                systemScreen_ = true;
                selectSystemScreen();
            }
            slide_ = page_ = 0;
            dirty_ = true;
        }
        if (hasToast_ && due(toastUntil_, now)) {
            hasToast_ = false;
            toast_.clear();
            dirty_ = true;
        }
        if (due(rotateAt_, now)) {
            rotateAt_ = 0;
            if (++page_ >= lastReport_.pages) {
                page_ = 0;
                if (lastReport_.slides > 1) slide_ = (slide_ + 1) % lastReport_.slides;
            }
            dirty_ = true;
        }
        if (due(flashSaveAt_, now)) flushSave();
        refreshClock();

        if (dirty_ && fontsReady_) {
            dirty_ = false;
            if (!render() && !systemScreen_) {
                // A restored screen that no longer builds: fall back.
                screen_.clear();
                systemScreen_ = true;
                selectSystemScreen();
                render();
            }
        }

        uint32_t wait = 1000;
        now = millis();
        for (uint32_t at : {rotateAt_, expireAt_, hasToast_ ? toastUntil_ : 0u, flashSaveAt_}) {
            if (at) wait = std::min<uint32_t>(wait, int32_t(at - now) > 0 ? at - now : 0);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::max<uint32_t>(wait, 10)));
    }
}

// Rejections are otherwise only visible to the MCP client; log them so a
// failed write can be diagnosed from the serial console.
void DisplayService::handle(Request& req) {
    // Read before handling: the spec is moved into the screen state.
    std::string type = req.spec["type"] | "";
    handleRequest(req);
    // Indexed by Kind.
    static const char* const NAMES[] = {"show",        "toast",         "clear",         "backlight",
                                        "canvas_draw", "canvas_update", "canvas_remove", "canvas_get"};
    const char* what = NAMES[int(req.kind)];
    const Result& res = req.result;
    if (!res.error.empty()) {
        Serial.printf("[Display] rejected %s %s: %s\n", what, type.c_str(), res.error.c_str());
    } else if (!res.report.warnings.empty()) {
        std::string joined;
        for (const auto& w : res.report.warnings) joined += (joined.empty() ? "" : "; ") + w;
        Serial.printf("[Display] %s %s warnings: %s\n", what, type.c_str(), joined.c_str());
    }
}

void DisplayService::handleRequest(Request& req) {
    Result& res = req.result;
    if (req.kind == Kind::Backlight) {
        backlight_ = req.flag;
        lcd_backlight(backlight_);
        xSemaphoreTake(lock_, portMAX_DELAY);
        status_.backlight = backlight_;
        xSemaphoreGive(lock_);
        res.ok = true;
        return;
    }
    if (!fontsReady_) {
        res.error = "fonts are not installed on the device; flash them with `pio run -t uploadfonts`";
        return;
    }

    switch (req.kind) {
        case Kind::CanvasDraw:
        case Kind::CanvasUpdate:
        case Kind::CanvasRemove:
        case Kind::CanvasGet:
            handleCanvas(req);
            break;
        case Kind::Show: {
            JsonDocument prev = std::move(screen_);
            bool prevSystem = systemScreen_;
            int prevSlide = slide_, prevPage = page_;
            screen_ = std::move(req.spec);
            systemScreen_ = false;
            slide_ = page_ = 0;
            if (!render(&res.report, &res.renderMs)) {
                res.error = res.report.error;
                screen_ = std::move(prev);
                systemScreen_ = prevSystem;
                slide_ = prevSlide;
                page_ = prevPage;
                dirty_ = true;
                return;
            }
            float duration = screen_["duration_s"] | 0.0f;
            expireAt_ = duration > 0 ? millis() + uint32_t(duration * 1000) : 0;
            // Timed screens are interruptions; they don't replace what is saved.
            if (duration <= 0 && (screen_["persist"] | true)) {
                // Too large for NVS: still show it, but drop the old saved
                // screen so a reboot can't bring back unrelated content.
                if (!save(screen_)) {
                    forgetSaved();
                    res.report.warnings.push_back("shown but not saved: larger than the 6144-byte limit for "
                                                  "restoring after a reboot");
                }
            } else if (duration <= 0) {
                forgetSaved();
            }
            res.ok = true;
            break;
        }
        case Kind::Toast: {
            SceneBuilder builder(fonts_, sys_);
            DisplayList probe;
            if (!builder.buildToast(req.spec, probe, res.report)) {
                res.error = res.report.error;
                return;
            }
            float duration = req.spec["duration_s"] | 5.0f;
            toast_ = std::move(req.spec);
            hasToast_ = true;
            toastUntil_ = millis() + uint32_t(std::max(1.0f, duration) * 1000);
            std::vector<std::string> warnings = res.report.warnings;
            render(nullptr, &res.renderMs);
            res.report.warnings = warnings;
            res.ok = true;
            break;
        }
        case Kind::Clear:
            screen_.clear();
            hasToast_ = false;
            systemScreen_ = true;
            expireAt_ = 0;
            forgetSaved();
            selectSystemScreen();
            res.ok = render(&res.report, &res.renderMs);
            break;
        default:
            break;
    }
}

void DisplayService::handleCanvas(Request& req) {
    Result& res = req.result;
    const JsonVariantConst args = req.spec.as<JsonVariantConst>();
    bool isCanvas = !systemScreen_ && !strcmp(screen_["type"] | "", "canvas");

    if (req.kind == Kind::CanvasGet) {
        if (isCanvas) res.data.set(screen_);
        res.elements = isCanvas ? int(screen_["elements"].size()) : 0;
        res.ok = true;
        return;
    }

    JsonDocument backup = psramDoc();
    backup.set(screen_);
    bool prevSystem = systemScreen_;
    if (req.kind == Kind::CanvasDraw && (!isCanvas || (args["clear"] | false))) {
        screen_.clear();
        screen_["type"] = "canvas";
        screen_["elements"].to<JsonArray>();
        isCanvas = true;
    }
    if (!isCanvas) {
        res.error = "nothing to edit: the screen is not a canvas, call canvas_draw first";
        return;
    }
    // Screen-wide settings can change with any operation; null removes them.
    for (JsonPairConst kv : args.as<JsonObjectConst>()) {
        const char* key = kv.key().c_str();
        if (strcmp(key, "theme") && strcmp(key, "background") && strcmp(key, "accent") && strcmp(key, "status_bar"))
            continue;
        if (kv.value().isNull())
            screen_.remove(key);
        else
            screen_[key] = kv.value();
    }

    JsonArray els = screen_["elements"];
    auto find = [&els](const char* id) -> int {
        for (size_t i = 0; id && i < els.size(); ++i)
            if (!strcmp(els[i]["id"] | "", id)) return int(i);
        return -1;
    };
    std::vector<std::string> warnings;

    if (req.kind == Kind::CanvasDraw) {
        for (JsonVariantConst e : args["elements"].as<JsonArrayConst>()) {
            int at = find(e["id"]);
            if (at >= 0) {
                els[at].set(e);
            } else if (els.size() < MAX_CANVAS_ELEMENTS) {
                els.add(e);
            } else {
                warnings.push_back("canvas is full (" + std::to_string(MAX_CANVAS_ELEMENTS) + " elements); some were dropped");
                break;
            }
        }
    } else if (req.kind == Kind::CanvasUpdate) {
        for (JsonVariantConst u : args["updates"].as<JsonArrayConst>()) {
            const char* id = u["id"];
            int at = find(id);
            if (at < 0) {
                warnings.push_back(std::string("no element with id '") + (id ? id : "") + "'");
                continue;
            }
            JsonObject el = els[at];
            for (JsonPairConst kv : u.as<JsonObjectConst>()) {
                if (!strcmp(kv.key().c_str(), "id")) continue;
                if (kv.value().isNull())
                    el.remove(kv.key().c_str());
                else
                    el[kv.key().c_str()] = kv.value();
            }
        }
    } else {
        if (args["all"] | false) {
            els.clear();
        } else {
            for (JsonVariantConst id : args["ids"].as<JsonArrayConst>()) {
                int at = find(id.as<const char*>());
                if (at >= 0)
                    els.remove(at);
                else
                    warnings.push_back(std::string("no element with id '") + (id.as<const char*>() ? id.as<const char*>() : "") + "'");
            }
        }
    }

    systemScreen_ = false;
    slide_ = page_ = 0;
    expireAt_ = 0;
    if (!render(&res.report, &res.renderMs)) {
        res.error = res.report.error;
        screen_.set(backup);
        systemScreen_ = prevSystem;
        dirty_ = true;
        return;
    }
    if (!save(screen_, true)) warnings.push_back("canvas is too large to survive a reboot (limit 6 KiB of JSON)");
    res.report.warnings.insert(res.report.warnings.end(), warnings.begin(), warnings.end());
    res.elements = int(els.size());
    res.ok = true;
}

void DisplayService::selectSystemScreen() {
    std::string type = sys_.wifi == SystemInfo::Wifi::Connected ? "home" : "setup";
    if (type != systemType_) dirty_ = true;
    systemType_ = type;
}

void DisplayService::refreshClock() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) return;  // not synced yet
    sys_.timeValid = true;
    sys_.year = t.tm_year + 1900;
    sys_.month = t.tm_mon + 1;
    sys_.day = t.tm_mday;
    sys_.weekday = t.tm_wday;
    sys_.hour = t.tm_hour;
    sys_.minute = t.tm_min;
    if (t.tm_min != lastMinute_) {
        lastMinute_ = t.tm_min;
        if (lastReport_.usesClock) dirty_ = true;
    }
}

bool DisplayService::render(SceneReport* report, uint32_t* ms) {
    uint32_t start = millis();
    SceneBuilder builder(fonts_, sys_);
    SceneReport rep;
    JsonDocument sysSpec;
    JsonVariantConst spec;
    if (systemScreen_) {
        sysSpec["type"] = systemType_;
        spec = sysSpec.as<JsonVariantConst>();
    } else {
        spec = screen_.as<JsonVariantConst>();
    }
    if (!builder.build(spec, slide_, page_, list_, rep)) {
        if (report) *report = rep;
        return false;
    }
    if (hasToast_) {
        SceneReport toastRep;
        builder.buildToast(toast_.as<JsonVariantConst>(), list_, toastRep);
    }
    page_ = std::min(page_, rep.pages - 1);
    slide_ = std::min(slide_, rep.slides - 1);

    lcd.startWrite();
    for (int y = 0; y < HEIGHT; y += bandHeight_) {
        int h = std::min(bandHeight_, HEIGHT - y);
        Canvas canvas(frame_, WIDTH, y, h);
        list_.render(canvas, fonts_);
        lcd.setAddrWindow(0, y, WIDTH, h);
        lcd.writePixels(frame_, WIDTH * h, true);
    }
    lcd.endWrite();

    lastReport_ = rep;
    scheduleRotation();
    uint32_t elapsed = millis() - start;
    if (report) *report = rep;
    if (ms) *ms = elapsed;

    xSemaphoreTake(lock_, portMAX_DELAY);
    status_.screen = systemScreen_ ? systemType_ : std::string(screen_["type"] | "text");
    JsonVariantConst cur = spec;
    if (!systemScreen_ && rep.slides > 1) cur = screen_["slides"][slide_];
    status_.title = cur["title"] | "";
    status_.slide = slide_;
    status_.slides = rep.slides;
    status_.page = page_;
    status_.pages = rep.pages;
    status_.remainingS = expireAt_ ? std::max<int32_t>(0, int32_t(expireAt_ - millis())) / 1000 : 0;
    xSemaphoreGive(lock_);
    Serial.printf("[Display] %s rendered in %u ms (glyph cache %u KiB, internal heap %u KiB free)\n",
                  status_.screen.c_str(), unsigned(elapsed), unsigned(fonts_.cacheBytes() / 1024),
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

void DisplayService::scheduleRotation() {
    rotateAt_ = 0;
    if (systemScreen_ || (lastReport_.pages <= 1 && lastReport_.slides <= 1)) return;
    float interval = screen_["interval_s"] | (lastReport_.slides > 1 ? 8.0f : 6.0f);
    if (lastReport_.slides > 1) {
        // A slide may override how long it stays up.
        interval = screen_["slides"][slide_]["interval_s"] | interval;
    }
    rotateAt_ = millis() + uint32_t(std::max(1.0f, interval) * 1000);
}

void DisplayService::loadSaved() {
    Preferences prefs;
    if (!prefs.begin(PREFS_NS, true)) return;
    size_t len = prefs.getBytesLength(PREFS_KEY);
    if (len > 0 && len <= MAX_SAVED_BYTES) {
        std::vector<char> buf(len);
        prefs.getBytes(PREFS_KEY, buf.data(), len);
        if (deserializeJson(saved_, buf.data(), len)) saved_.clear();
    }
    prefs.end();
}

bool DisplayService::save(const JsonDocument& spec, bool deferFlash) {
    size_t bytes = measureJson(spec);
    if (bytes > MAX_SAVED_BYTES) {
        Serial.printf("[Display] screen not saved: %u bytes > %u\n", unsigned(bytes), unsigned(MAX_SAVED_BYTES));
        return false;
    }
    saved_ = spec;
    if (deferFlash) {
        flashSaveAt_ = millis() + FLASH_SAVE_DELAY_MS;
    } else {
        flushSave();
    }
    return true;
}

void DisplayService::flushSave() {
    flashSaveAt_ = 0;
    if (saved_.isNull()) return;
    std::string json;
    serializeJson(saved_, json);
    Preferences prefs;
    if (!prefs.begin(PREFS_NS, false)) return;
    prefs.putBytes(PREFS_KEY, json.data(), json.size());
    prefs.end();
}

void DisplayService::forgetSaved() {
    saved_.clear();
    flashSaveAt_ = 0;
    Preferences prefs;
    if (!prefs.begin(PREFS_NS, false)) return;
    prefs.remove(PREFS_KEY);
    prefs.end();
}
