#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <deque>
#include <memory>
#include <string>

#include "PsramJson.h"
#include "gfx/DisplayList.h"
#include "gfx/FontEngine.h"
#include "gfx/Scenes.h"

/* Owns the LCD, the font engine and what is on screen. Everything that draws
 * runs on one FreeRTOS task; MCP tool handlers (HTTP worker task) and the main
 * loop only submit requests, the same "submit, don't touch" rule the network
 * manager follows. Requests are answered once the frame is on the glass so
 * the tool result can report layout warnings and page counts. */
class DisplayService {
public:
    struct Result {
        bool ok = false;
        std::string error;
        SceneReport report;
        uint32_t renderMs = 0;
        int elements = -1;   // canvas element count after the operation
        JsonDocument data{PsramJsonAllocator::instance()};  // canvas_get payload
    };

    struct Status {
        std::string screen;  // spec type, or "home"/"setup" for system screens
        std::string title;
        int slide = 0, slides = 1, page = 0, pages = 1;
        bool backlight = true;
        bool fontsReady = false;
        bool fullFrame = false;  // false: rendered in strips (no PSRAM)
        uint32_t remainingS = 0;  // until the screen times out, 0 = stays
    };

    bool begin();

    // Called from tool handlers; block until rendered or `timeoutMs` elapses.
    Result show(JsonDocument spec, uint32_t timeoutMs = 8000);
    Result toast(JsonDocument spec, uint32_t timeoutMs = 8000);
    Result clear(uint32_t timeoutMs = 8000);
    Result setBacklight(bool on, uint32_t timeoutMs = 2000);

    /* Canvas: a screen of id-addressable elements edited incrementally.
     * draw adds or replaces (by id), update merges fields into elements by id,
     * remove deletes by id; get returns the current canvas. */
    Result canvasDraw(JsonDocument args, uint32_t timeoutMs = 8000);
    Result canvasUpdate(JsonDocument args, uint32_t timeoutMs = 8000);
    Result canvasRemove(JsonDocument args, uint32_t timeoutMs = 8000);
    Result canvasGet(uint32_t timeoutMs = 2000);

    // Main loop: publish network state for the system screens and status bar.
    void updateSystem(const SystemInfo& info);
    Status status();

private:
    enum class Kind : uint8_t { Show, Toast, Clear, Backlight, CanvasDraw, CanvasUpdate, CanvasRemove, CanvasGet };
    struct Request {
        Kind kind;
        JsonDocument spec{PsramJsonAllocator::instance()};
        bool flag = false;
        Result result;
        SemaphoreHandle_t done = nullptr;
        ~Request() {
            if (done) vSemaphoreDelete(done);
        }
    };

    Result submit(std::shared_ptr<Request> req, uint32_t timeoutMs);
    static void taskEntry(void* arg);
    void run();
    void handle(Request& req);
    void handleRequest(Request& req);
    void handleCanvas(Request& req);
    bool mapFonts();
    void showFontError();
    void refreshClock();
    void selectSystemScreen();
    // Builds the current screen (+ toast) and pushes it to the panel.
    bool render(SceneReport* report = nullptr, uint32_t* ms = nullptr);
    void scheduleRotation();
    void loadSaved();
    // Remembers `spec` for restore; the flash write can be deferred so rapid
    // canvas edits don't wear NVS. Returns false when it is too large.
    bool save(const JsonDocument& spec, bool deferFlash = false);
    void flushSave();
    void forgetSaved();

    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;  // guards queue_, pendingSys_ and status_
    std::deque<std::shared_ptr<Request>> queue_;
    SystemInfo pendingSys_;
    bool sysDirty_ = false;
    Status status_;

    // ---- display task only ----
    FontEngine fonts_;
    SystemInfo sys_;
    DisplayList list_;
    uint16_t* frame_ = nullptr;
    int bandHeight_ = 0;
    bool fontsReady_ = false;
    bool backlight_ = true;

    JsonDocument screen_{PsramJsonAllocator::instance()};  // user screen; null = system screen
    JsonDocument saved_{PsramJsonAllocator::instance()};   // persisted user screen, restored once online
    JsonDocument toast_{PsramJsonAllocator::instance()};
    uint32_t flashSaveAt_ = 0;
    bool hasToast_ = false;
    bool systemScreen_ = true;
    std::string systemType_;
    int slide_ = 0, page_ = 0;
    SceneReport lastReport_;
    uint32_t rotateAt_ = 0, expireAt_ = 0, toastUntil_ = 0;
    int lastMinute_ = -1;
    bool dirty_ = true;
};

extern DisplayService display;
