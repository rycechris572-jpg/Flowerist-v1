#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace geode::prelude;

namespace pib {
    struct Event {
        PlayerButton button;
        bool pressed;
        double time;
    };

    class Engine {
    public:
        static constexpr size_t Capacity = 256;
        std::array<Event, Capacity> events{};
        size_t next = 0;
        size_t count = 0;
        double clock = 0.0;
        double lastPress = -1.0;
        double meanInterval = 0.0;
        double predictedNextPress = -1.0;
        uint64_t presses = 0;
        uint64_t releases = 0;
        bool active = false;

        void reset() {
            next = 0;
            count = 0;
            clock = 0.0;
            lastPress = -1.0;
            meanInterval = 0.0;
            predictedNextPress = -1.0;
            presses = 0;
            releases = 0;
            active = true;
        }

        void tick(float dt) {
            if (!active || !std::isfinite(dt) || dt <= 0.f) return;
            // Bound a single update to avoid enormous timing spikes after a pause.
            clock += std::min(static_cast<double>(dt), 0.25);
        }

        void record(PlayerButton button, bool pressed) {
            if (!active) return;
            events[next] = {button, pressed, clock};
            next = (next + 1) % Capacity;
            count = std::min(count + 1, Capacity);
            if (pressed) {
                ++presses;
                if (lastPress >= 0.0) {
                    const double interval = clock - lastPress;
                    // Ignore zero-time duplicates and gaps that cannot model a rhythm.
                    if (interval >= 0.005 && interval <= 2.0) {
                        meanInterval = meanInterval == 0.0
                            ? interval : meanInterval * 0.75 + interval * 0.25;
                    }
                }
                lastPress = clock;
                predictedNextPress = meanInterval > 0.0 ? clock + meanInterval : -1.0;
            } else {
                ++releases;
            }
        }
    };

    Engine& engine() {
        static Engine instance;
        return instance;
    }
}

class $modify(PIBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        pib::engine().reset();
        log::info("PIB V1.0 engine started (capture/estimate only)");
        return true;
    }

    void update(float dt) {
        PlayLayer::update(dt);
        pib::engine().tick(dt);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        pib::engine().reset();
    }

    void onQuit() {
        pib::engine().active = false;
        PlayLayer::onQuit();
    }
};

class $modify(PIBPlayerObject, PlayerObject) {
    void pushButton(PlayerButton button) {
        pib::engine().record(button, true);
        PlayerObject::pushButton(button);
    }

    void releaseButton(PlayerButton button) {
        pib::engine().record(button, false);
        PlayerObject::releaseButton(button);
    }
};
