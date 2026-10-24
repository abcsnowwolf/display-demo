#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <deque>
#include <memory>
#include <string>

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

    // Main loop: publish network state for the system screens and status bar.
    void updateSystem(const SystemInfo& info);
    Status status();

private:
    enum class Kind : uint8_t { Show, Toast, Clear, Backlight };
    struct Request {
        Kind kind;
        JsonDocument spec;
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
    bool mapFonts();
    void showFontError();
    void refreshClock();
    void selectSystemScreen();
    // Builds the current screen (+ toast) and pushes it to the panel.
    bool render(SceneReport* report = nullptr, uint32_t* ms = nullptr);
    void scheduleRotation();
    void loadSaved();
    void save(const JsonDocument& spec);
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

    JsonDocument screen_;   // user screen; null = system screen
    JsonDocument saved_;    // persisted user screen, restored once online
    JsonDocument toast_;
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
