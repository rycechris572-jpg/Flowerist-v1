#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/cocos/touch_dispatcher/CCTouchDispatcher.h>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Windows.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

// ============================================================
// FLOWERIST — SETTINGS
// ============================================================

constexpr int SPAM_INTERVAL_TICKS = 8;
constexpr int SPAM_STREAK_REQUIRED = 3;

static void refreshBufferHud();
static void refreshFloweristConsole();
static void toggleFloweristConsole();

class TrajectoryBufferSim {
private:
    PlayerObject* m_simPlayer1 = nullptr;
    PlayerObject* m_simPlayer2 = nullptr;

    bool m_simulating = false;
    bool m_firingBuffered = false;
    bool m_simulationDead = false;

    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    int  m_presetIndex = 0;
    bool m_enabled = true;
    int  m_maxTicksNormal = 2;
    int  m_maxTicksWave = 2;

    int  m_waitTicks = -1;
    bool m_bufferedPush = true;
    bool m_queuedOpposite = false;
    float m_frameDt = 0.25f;

    int m_ticksSinceLastInput = 999;
    int m_rapidInputStreak = 0;

    PlayerObject* createTrajectoryPlayer() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return nullptr;

        PlayerObject* player =
            PlayerObject::create(1, 1, pl, pl, true);

        if (!player) return nullptr;
        if (!pl->m_objectLayer) {
            // PlayerObject::create returns an autoreleased object; do not release it here.
            return nullptr;
        }

        player->retain();
        player->setPosition({0, 105});
        player->setVisible(false);
        pl->m_objectLayer->addChild(player);
        return player;
    }

    void resetCollisionLog(PlayerObject* self) {
        if (!self) return;

        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    void syncSimPlayer(PlayerObject* sim, PlayerObject* real) {
        if (!sim || !real) return;

        sim->setVisible(false);
        sim->copyAttributes(real);
        sim->setScale(real->getScale());
        sim->setContentSize(real->getContentSize());
        sim->m_width = real->m_width;
        sim->m_height = real->m_height;
        sim->m_gravityMod = real->m_gravityMod;
        sim->m_isOnGround = real->m_isOnGround;
        sim->m_isUpsideDown = real->m_isUpsideDown;
        sim->m_vehicleSize = real->m_vehicleSize;
        sim->m_playerSpeed = real->m_playerSpeed;
    }

    float getValidDt() const {
        if (m_frameDt > 0.001f && m_frameDt < 2.0f) {
            return m_frameDt;
        }
        return 0.25f;
    }

public:
    void init() {
        // Clear a previous run before creating simulation objects for a new level.
        quit();
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !pl->m_objectLayer) return;

        m_simPlayer1 = createTrajectoryPlayer();
        m_simPlayer2 = createTrajectoryPlayer();

        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_physicalHolding = false;
        m_gameHolding = false;
        m_simulating = false;
        m_firingBuffered = false;
        m_simulationDead = false;
        m_ticksSinceLastInput = 999;
        m_rapidInputStreak = 0;
    }

    void quit() {
        if (m_simPlayer1) {
            m_simPlayer1->removeFromParentAndCleanup(true);
            m_simPlayer1->release();
            m_simPlayer1 = nullptr;
        }

        if (m_simPlayer2) {
            m_simPlayer2->removeFromParentAndCleanup(true);
            m_simPlayer2->release();
            m_simPlayer2 = nullptr;
        }

        m_simulating = false;
        m_firingBuffered = false;
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_rapidInputStreak = 0;
    }

    void selectPreset(int index) {
        m_presetIndex = std::clamp(index, 0, 9);
        constexpr int normal[] = {2,4,8,16,24,32,48,54,60,0};
        constexpr int wave[] = {2,4,8,8,24,32,48,54,60,0};
        m_enabled = m_presetIndex != 9;
        m_maxTicksNormal = normal[m_presetIndex];
        m_maxTicksWave = wave[m_presetIndex];
        m_waitTicks = -1;
        m_queuedOpposite = false;
    }
    void cyclePreset() {
        constexpr int normalTicks[] = {
            2, 4, 8, 16, 24, 32, 48, 54, 60, 0
        };
        constexpr int waveTicks[] = {
            2, 4, 8, 8, 24, 32, 48, 54, 60, 0
        };

        m_presetIndex = (m_presetIndex + 1) % 10;
        m_waitTicks = -1;
        m_queuedOpposite = false;

        m_enabled = m_presetIndex != 9;
        m_maxTicksNormal = normalTicks[m_presetIndex];
        m_maxTicksWave = waveTicks[m_presetIndex];

        if (m_enabled) {
            printf(
                "[Flowerist] F6 -> STAGE %d: %d Normal / %d Wave ticks\n",
                m_presetIndex + 1,
                m_maxTicksNormal,
                m_maxTicksWave
            );
        } else {
            printf("[Flowerist] F6 -> OFF\n");
        }
    }

    int normalTicks() const { return m_maxTicksNormal; }
    int waveTicks() const { return m_maxTicksWave; }
    bool enabled() const { return m_enabled; }
    bool isSimulating() const { return m_simulating; }
    bool isFiringBuffered() const { return m_firingBuffered; }

    bool checkSimulationDeath(PlayerObject* player) {
        if (m_simulating &&
            (player == m_simPlayer1 || player == m_simPlayer2)) {
            m_simulationDead = true;
            return true;
        }

        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_gameHolding = false;
        m_rapidInputStreak = 0;
        return false;
    }

    void setFrameDelta(float dt) {
        PlayLayer* pl = PlayLayer::get();

        if (pl && pl->m_gameState.m_timeWarp > 0.f) {
            m_frameDt = dt / pl->m_gameState.m_timeWarp;
        } else {
            m_frameDt = dt;
        }
    }

    int getSurvivalTicks(
        PlayLayer* pl,
        PlayerObject* realP1,
        PlayerObject* realP2,
        bool isDual,
        int delayFrames,
        bool actionPush,
        int maxTicks
    ) {
        if (!pl || !m_simPlayer1 || !realP1) {
            return maxTicks;
        }
maxTicks = std::clamp(maxTicks, 0, 60);
delayFrames = std::clamp(delayFrames, 0, 60);
        m_simulating = true;
        m_simulationDead = false;

        syncSimPlayer(m_simPlayer1, realP1);

        if (isDual && m_simPlayer2 && realP2) {
            syncSimPlayer(m_simPlayer2, realP2);
        }

        if (delayFrames > 0) {
            if (!actionPush) {
                m_simPlayer1->pushButton(PlayerButton::Jump);
                if (isDual && m_simPlayer2) {
                    m_simPlayer2->pushButton(PlayerButton::Jump);
                }
            } else {
                m_simPlayer1->releaseButton(PlayerButton::Jump);
                if (isDual && m_simPlayer2) {
                    m_simPlayer2->releaseButton(PlayerButton::Jump);
                }
            }
        }

        float dt = getValidDt();
        int survived = 0;

        bool isContinuousMode =
            realP1->m_isDart ||
            realP1->m_isBird ||
            realP1->m_isSwing ||
            (isDual && realP2 &&
                (realP2->m_isDart ||
                 realP2->m_isBird ||
                 realP2->m_isSwing));

        int airborneTicks = 0;
        int safeLandedTicks = 0;

        for (int i = 0; i < delayFrames + maxTicks; i++) {
            if (i == delayFrames) {
                if (actionPush) {
                    m_simPlayer1->pushButton(PlayerButton::Jump);
                    if (isDual && m_simPlayer2) {
                        m_simPlayer2->pushButton(PlayerButton::Jump);
                    }
                } else {
                    m_simPlayer1->releaseButton(PlayerButton::Jump);
                    if (isDual && m_simPlayer2) {
                        m_simPlayer2->releaseButton(PlayerButton::Jump);
                    }
                }
            }

            m_simPlayer1->update(dt);
            m_simPlayer1->updateRotation(dt);

            if (isDual && m_simPlayer2) {
                m_simPlayer2->update(dt);
                m_simPlayer2->updateRotation(dt);
            }

            if (m_simulationDead) break;

            resetCollisionLog(m_simPlayer1);
            pl->checkCollisions(m_simPlayer1, dt, false);
            if (m_simulationDead) break;

            if (isDual && m_simPlayer2 && realP2) {
                resetCollisionLog(m_simPlayer2);
                pl->checkCollisions(m_simPlayer2, dt, false);
                if (m_simulationDead) break;
            }

            if (i >= delayFrames) {
                survived++;

                if (!isContinuousMode) {
                    bool p1Grounded = m_simPlayer1->m_isOnGround;
                    bool p2Grounded =
                        (!isDual || !m_simPlayer2)
                            ? true
                            : m_simPlayer2->m_isOnGround;

                    if (!p1Grounded || !p2Grounded) {
                        airborneTicks++;
                        safeLandedTicks = 0;
                    } else if (airborneTicks >= 15) {
                        safeLandedTicks++;

                        if (safeLandedTicks >= 10) {
                            survived = maxTicks;
                            break;
                        }
                    }
                }
            }
        }

        m_simulating = false;
        return survived;
    }

    bool interceptInput(
        GJBaseGameLayer* layer,
        bool push,
        bool isPlayer1
    ) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;

        bool isDual =
            pl->m_gameState.m_isDualMode &&
            pl->m_player2 != nullptr;

        bool isTwoPlayerControl =
            pl->m_level && pl->m_level->m_twoPlayerMode;

        if (!isPlayer1) {
            if (isDual && !isTwoPlayerControl &&
                (m_waitTicks >= 0 || m_waitTicks == -2)) {
                return false;
            }
            return true;
        }

        m_physicalHolding = push;

        if (!m_enabled) {
            m_gameHolding = push;
            return true;
        }

        PlayerObject* p1 = layer->m_player1;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;

        if (!p1 || p1->m_isDead) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        if (p1->m_isShip || (isDual && p2 && p2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        if (m_waitTicks >= 0) {
            if (push != m_bufferedPush) {
                m_queuedOpposite = true;
            } else {
                m_queuedOpposite = false;
            }

            m_ticksSinceLastInput = 0;
            return false;
        }

        if (m_waitTicks == -2) {
            m_waitTicks = -1;
            m_queuedOpposite = false;

            if (m_gameHolding != !m_bufferedPush) {
                m_SimFireButton(pl, !m_bufferedPush);
            }
        }

        bool isWave =
            p1->m_isDart ||
            (isDual && p2 && p2->m_isDart);

        bool isMiniWave =
            (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
            (isDual && p2 && p2->m_isDart &&
             p2->m_vehicleSize < 1.0f);

        if (m_ticksSinceLastInput < SPAM_INTERVAL_TICKS) {
            m_rapidInputStreak++;
        } else {
            m_rapidInputStreak = 0;
        }

        m_ticksSinceLastInput = 0;

        if (isWave &&
            m_rapidInputStreak >= SPAM_STREAK_REQUIRED) {
            m_gameHolding = push;
            return true;
        }

        bool releaseAffectsPhysics =
            p1->m_isDart ||
            p1->m_isRobot ||
            (isDual && p2 &&
             (p2->m_isDart || p2->m_isRobot));

        if (!push && !releaseAffectsPhysics) {
            m_gameHolding = false;
            return true;
        }

        m_queuedOpposite = false;
        const char* act = push ? "PRESS" : "RELEASE";

        int targetHorizon = isMiniWave ? 50 : 60;

        int maxDelayFrames =
            isWave ? m_maxTicksWave : m_maxTicksNormal;

        int ticksFrame0 = getSurvivalTicks(
            pl, p1, p2, isDual, 0, push, targetHorizon
        );

        if (ticksFrame0 >= targetHorizon) {
            m_gameHolding = push;
            return true;
        }

        int bestDelay = 0;
        int bestTicks = ticksFrame0;

        const int boundedMaxDelay = std::clamp(maxDelayFrames, 0, 60);
const int candidateCount = std::min(boundedMaxDelay, 16);
const int delayStep = candidateCount > 0
    ? std::max(1, (boundedMaxDelay + candidateCount - 1) / candidateCount)
    : 1;

for (int delay = 1; delay <= boundedMaxDelay; delay += delayStep) {

            int ticks = getSurvivalTicks(
                pl, p1, p2, isDual, delay, push, targetHorizon
            );

            if (isWave) {
                int minClearedGain = (delay * 2) + 10;

                if (ticks >= targetHorizon &&
                    (ticks - ticksFrame0) >= (delay * 2) + 6) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }

                if (ticks >= ticksFrame0 + minClearedGain &&
                    ticks > bestTicks + 2) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    if (ticks >= targetHorizon) break;
                }
            } else {
                if (ticks >= targetHorizon) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }

                if (ticks > bestTicks + 12) {
                    bestDelay = delay;
                    bestTicks = ticks;
                }
            }
        }

        if (bestDelay > 0) {
            printf(
                "[Flowerist] SAVED +%d TICKS (%s): %d -> %d [Limit: %d]\n",
                bestDelay,
                act,
                ticksFrame0,
                bestTicks,
                maxDelayFrames
            );

            m_bufferedPush = push;
            m_waitTicks = bestDelay;
            return false;
        }

        m_gameHolding = push;
        return true;
    }
    void onPlayerUpdate(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || player != pl->m_player1) return;

        // F6 always works, including when the buffer is OFF.
        if (GetAsyncKeyState(VK_F6) & 1) {
            cyclePreset();
            refreshBufferHud();
            refreshFloweristConsole();
        }

        if (!m_enabled) return;

        if (m_ticksSinceLastInput < 999) {
            m_ticksSinceLastInput++;
        }

        if (player->m_isShip ||
            (pl->m_gameState.m_isDualMode &&
             pl->m_player2 && pl->m_player2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;

            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
            return;
        }

        if (m_waitTicks == 0) {
            m_SimFireButton(pl, m_bufferedPush);

            if (m_queuedOpposite) {
                m_waitTicks = -2;
                m_queuedOpposite = false;
            } else {
                m_waitTicks = -1;
            }
        } else if (m_waitTicks > 0) {
            m_waitTicks--;
        } else if (m_waitTicks == -2) {
            m_SimFireButton(pl, !m_bufferedPush);
            m_queuedOpposite = false;
            m_waitTicks = -1;
        } else if (m_waitTicks == -1) {
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
        }
    }

    void m_SimFireButton(PlayLayer* pl, bool push) {
        if (!pl) return;

        m_firingBuffered = true;
        m_gameHolding = push;

        pl->handleButton(push, 1, true);

        if (pl->m_gameState.m_isDualMode &&
            (!pl->m_level || !pl->m_level->m_twoPlayerMode)) {
            pl->handleButton(push, 1, false);
        }

        m_firingBuffered = false;
    }
};

// ============================================================
// FLOWERIST GUI — scene-level overlay, opened only by F2
// ============================================================

static TrajectoryBufferSim s_sim;
static cocos2d::CCLabelBMFont* s_bufferHud = nullptr;
static bool s_consoleOpen = false;
static bool s_reduceMotion = false;
static bool s_showBufferHud = true;
static int s_activeTab = 0;
static int s_stageSelection = 0;
static int s_selectedMode = 0;
static std::string s_lastLog = "Flowerist loaded — waiting for input";
static const char* s_tabs[] = {"OVERVIEW", "STAGES", "MODES", "TUNING", "ASSISTS", "OPTIMIZE", "SETTINGS", "COMPETE"};
static const char* s_tabDescriptions[] = {
    "Live buffer state and recent activity",
    "Choose a ready-made prediction profile",
    "Browse game-mode profiles",
    "Adjust the prediction horizon",
    "See which input assists are active",
    "Performance and simulation safeguards",
    "Customize the Flowerist overlay",
    "Competitive profiles and presets"
};
static const char* s_modeNames[] = {"CUBE", "SHIP", "BALL", "UFO", "WAVE", "ROBOT", "SPIDER", "SWING"};
static const char* s_presetNames[] = {"STAGE 1", "STAGE 2", "STAGE 3", "STAGE 4", "STAGE 5", "STAGE 6", "STAGE 7", "STAGE 8", "STAGE 9", "OFF"};
static const int s_presetTicks[] = {2, 4, 8, 16, 24, 32, 48, 54, 60, 0};

static cocos2d::CCLayerColor* s_guiDimmer = nullptr;
static cocos2d::CCLayerColor* s_consolePanel = nullptr;
static cocos2d::CCLayerColor* s_guiNavHighlight = nullptr;
static cocos2d::CCNode* s_guiFlower = nullptr;
static cocos2d::CCLabelBMFont* s_consoleTitle = nullptr;
static cocos2d::CCLabelBMFont* s_consoleStatus = nullptr;
static cocos2d::CCLabelBMFont* s_guiTabTitle = nullptr;
static cocos2d::CCLabelBMFont* s_guiTabDescription = nullptr;
static cocos2d::CCLabelBMFont* s_guiBody = nullptr;
static cocos2d::CCLabelBMFont* s_guiLog = nullptr;
static cocos2d::CCLabelBMFont* s_guiFooter = nullptr;
static cocos2d::CCMenu* s_guiNavMenu = nullptr;
static cocos2d::CCMenu* s_closeMenu = nullptr;
static cocos2d::CCMenu* s_stageMenu = nullptr;
static cocos2d::CCMenu* s_modesMenu = nullptr;
static cocos2d::CCMenu* s_tuningMenu = nullptr;
static cocos2d::CCMenu* s_settingsMenu = nullptr;
static cocos2d::CCMenuItemLabel* s_navItems[8] = {};
static cocos2d::CCLayer* s_guiPages[8] = {};
static cocos2d::CCLayerColor* s_modeCards[8] = {};
static cocos2d::CCLayerColor* s_stageCards[6] = {};
static cocos2d::CCLayerColor* s_presetCards[10] = {};
static float s_guiWidth = 820.f;
static float s_guiHeight = 500.f;
static float s_guiNavWidth = 174.f;
static float s_guiContentWidth = 646.f;

static void renderFloweristTab();
static void refreshFloweristConsole();
static void cleanupFloweristConsole();
static void toggleFloweristConsole();
static bool createFloweristConsole();
static void setFloweristMenusEnabled(bool enabled);
static void updateFloweristFlowerAnimation();

// This transparent input shield keeps clicks on the overlay from reaching pause buttons behind it.
class FloweristBackdrop : public cocos2d::CCLayerColor {
public:
    static FloweristBackdrop* create(cocos2d::ccColor4B color, float width, float height) {
        auto* layer = new FloweristBackdrop();
        if (layer && layer->initWithColor(color, width, height)) {
            layer->autorelease();
            layer->setTouchEnabled(true);
            layer->setTouchMode(cocos2d::kCCTouchesOneByOne);
            layer->setTouchPriority(-129);
            return layer;
        }
        CC_SAFE_DELETE(layer);
        return nullptr;
    }
    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, this->getTouchPriority(), true);
    }
    bool ccTouchBegan(cocos2d::CCTouch*, cocos2d::CCEvent*) override { return true; }
    void ccTouchMoved(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}
    void ccTouchEnded(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}
};

static cocos2d::CCLabelBMFont* floweristLabel(
    cocos2d::CCNode* parent, const std::string& text,
    float x, float y, float scale, cocos2d::ccColor3B color,
    bool leftAligned = false, const char* font = "bigFont.fnt"
) {
    if (!parent) return nullptr;
    auto* label = cocos2d::CCLabelBMFont::create(text.c_str(), font);
    if (!label) return nullptr;
    label->setScale(scale);
    label->setColor(color);
    label->setAnchorPoint(leftAligned ? cocos2d::CCPoint{0.f, 0.5f} : cocos2d::CCPoint{0.5f, 0.5f});
    label->setPosition({x, y});
    parent->addChild(label, 20);
    return label;
}

static cocos2d::CCLayerColor* floweristRect(
    cocos2d::CCNode* parent, float x, float y, float w, float h,
    cocos2d::ccColor4B color, int z = 0
) {
    if (!parent || w <= 0.f || h <= 0.f) return nullptr;
    auto* rect = cocos2d::CCLayerColor::create(color, w, h);
    if (!rect) return nullptr;
    rect->setPosition({x, y});
    parent->addChild(rect, z);
    return rect;
}

static std::string floweristWrapText(const std::string& text, size_t maxChars) {
    std::istringstream input(text);
    std::string word;
    std::string line;
    std::string result;
    maxChars = std::max<size_t>(18, maxChars);

    while (input >> word) {
        if (!line.empty() && line.size() + 1 + word.size() > maxChars) {
            if (!result.empty()) result += "\n";
            result += line;
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += word;
    }
    if (!line.empty()) {
        if (!result.empty()) result += "\n";
        result += line;
    }
    return result;
}

static cocos2d::CCLayerColor* floweristCard(
    cocos2d::CCNode* parent, float x, float y, float w, float h,
    const std::string& title, const std::string& body,
    cocos2d::ccColor4B accent = {255, 93, 174, 255}
) {
    auto* card = floweristRect(parent, x, y, w, h, {38, 22, 40, 255});
    if (!card) return nullptr;
    floweristRect(card, 0.f, h - 2.f, w, 2.f, accent);
    floweristLabel(card, title, 13.f, h - 22.f, 0.235f, {255, 142, 200}, true);
    if (!body.empty()) {
        const float bodyScale = 0.225f;
        const auto maxChars = static_cast<size_t>(std::max(18.f, (w - 26.f) / 4.6f));
        const std::string bodyText = (h > 64.f) ? floweristWrapText(body, maxChars) : body;
        const float descY = h > 64.f ? h - 45.f : std::max(13.f, h - 32.f);
        auto* desc = floweristLabel(card, bodyText, 13.f, descY, bodyScale, {232, 211, 228}, true, "chatFont.fnt");
        if (desc) desc->setAnchorPoint({0.f, 1.f});
    }
    return card;
}

static void appendFloweristLog(const std::string& event) {
    s_lastLog = event;
    if (s_activeTab == 0 && s_guiLog) s_guiLog->setString(s_lastLog.c_str());
    if (s_consoleStatus) refreshFloweristConsole();
}

class FloweristGuiActions : public cocos2d::CCObject {
public:
    static FloweristGuiActions* create() {
        auto* obj = new FloweristGuiActions();
        if (obj) obj->autorelease();
        return obj;
    }
    void tab(cocos2d::CCObject* sender) {
        if (!sender) return;
        const int nextTab = std::clamp(static_cast<cocos2d::CCNode*>(sender)->getTag(), 0, 7);
        if (nextTab == s_activeTab) {
            // Re-render the active page but leave its animations alone on repeated taps.
            renderFloweristTab();
            return;
        }
        s_activeTab = nextTab;
        renderFloweristTab();
        // A tiny title/description fade gives feedback without animating layout or input hitboxes.
        if (!s_reduceMotion) {
            if (s_guiTabTitle) {
                s_guiTabTitle->stopAllActions();
                s_guiTabTitle->setOpacity(150);
                s_guiTabTitle->runAction(cocos2d::CCFadeTo::create(0.14f, 255));
            }
            if (s_guiTabDescription) {
                s_guiTabDescription->stopAllActions();
                s_guiTabDescription->setOpacity(150);
                s_guiTabDescription->runAction(cocos2d::CCFadeTo::create(0.16f, 255));
            }
        }
    }
    void stage(cocos2d::CCObject* sender) {
        if (!sender) return;
        const int selected = std::clamp(static_cast<cocos2d::CCNode*>(sender)->getTag(), 0, 5);
        s_stageSelection = selected;
        // Stage buttons map to the existing predictor presets; CUSTOM is a visual selection only for now.
        const int presetIndex[] = {0, 2, 3, 4, 6, -1};
        if (presetIndex[selected] >= 0) s_sim.selectPreset(presetIndex[selected]);
        appendFloweristLog(selected == 5 ? "Selected CUSTOM profile (manual tuning is not implemented yet)" : fmt::format("Selected {} profile", selected + 1));
        refreshBufferHud();
        refreshFloweristConsole();
        renderFloweristTab();
    }
    void mode(cocos2d::CCObject* sender) {
        if (!sender) return;
        s_selectedMode = std::clamp(static_cast<cocos2d::CCNode*>(sender)->getTag(), 0, 7);
        appendFloweristLog(fmt::format("Selected {} profile preview", s_modeNames[s_selectedMode]));
        renderFloweristTab();
    }
    void preset(cocos2d::CCObject* sender) {
        if (!sender) return;
        const int index = std::clamp(static_cast<cocos2d::CCNode*>(sender)->getTag(), 0, 9);
        s_sim.selectPreset(index);
        appendFloweristLog(index == 9 ? "Predictive buffer switched off" : fmt::format("Preset changed to {} ({} tick lookahead)", s_presetNames[index], s_presetTicks[index]));
        refreshBufferHud();
        refreshFloweristConsole();
        renderFloweristTab();
    }
    void setting(cocos2d::CCObject* sender) {
        if (!sender) return;
        int settingTag = static_cast<cocos2d::CCNode*>(sender)->getTag();
        if (settingTag == 0) {
            s_reduceMotion = !s_reduceMotion;
            updateFloweristFlowerAnimation();
            appendFloweristLog(std::string("Reduced motion ") + (s_reduceMotion ? "enabled" : "disabled"));
        } else if (settingTag == 1) {
            s_showBufferHud = !s_showBufferHud;
            if (s_bufferHud) s_bufferHud->setVisible(s_showBufferHud);
            appendFloweristLog(std::string("Buffer HUD ") + (s_showBufferHud ? "shown" : "hidden"));
        }
        renderFloweristTab();
    }
    void close(cocos2d::CCObject*) {
        s_consoleOpen = false;
        if (s_consolePanel) {
            s_consolePanel->stopAllActions();
            s_consolePanel->setScale(1.f);
        }
        if (s_guiDimmer) {
            s_guiDimmer->setVisible(false);
            s_guiDimmer->setTouchEnabled(false);
        }
        setFloweristMenusEnabled(false);
        updateFloweristFlowerAnimation();
    }
};

static FloweristGuiActions* s_guiActions = nullptr;

static void setFloweristMenusEnabled(bool enabled) {
    // Hidden page menus used to remain touch-enabled and could swallow taps meant for tabs.
    if (s_guiNavMenu) s_guiNavMenu->setTouchEnabled(enabled);
    if (s_closeMenu) s_closeMenu->setTouchEnabled(enabled);
    if (s_stageMenu) s_stageMenu->setTouchEnabled(enabled && s_activeTab == 1);
    if (s_modesMenu) s_modesMenu->setTouchEnabled(enabled && s_activeTab == 2);
    if (s_tuningMenu) s_tuningMenu->setTouchEnabled(enabled && s_activeTab == 3);
    if (s_settingsMenu) s_settingsMenu->setTouchEnabled(enabled && s_activeTab == 6);
}

static void updateFloweristFlowerAnimation() {
    if (!s_guiFlower) return;
    s_guiFlower->stopAllActions();
    s_guiFlower->setRotation(0.f);
    if (s_consoleOpen && !s_reduceMotion) {
        // One slow action on a tiny decorative node; no update loop or particles.
        s_guiFlower->runAction(cocos2d::CCRepeatForever::create(
            cocos2d::CCRotateBy::create(24.f, 360.f)));
    }
}

static cocos2d::CCMenuItemLabel* floweristMenuLabel(
    cocos2d::CCMenu* menu, FloweristGuiActions* actions,
    const std::string& text, int tag, cocos2d::CCPoint position,
    cocos2d::SEL_MenuHandler handler, float scale = 0.22f,
    cocos2d::ccColor3B color = {245, 218, 235}
) {
    if (!menu || !actions) return nullptr;
    auto* label = cocos2d::CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    if (!label) return nullptr;
    label->setScale(scale);
    label->setColor(color);
    auto* item = cocos2d::CCMenuItemLabel::create(label, actions, handler);
    if (!item) return nullptr;
    item->setTag(tag);
    item->setPosition(position);
    menu->addChild(item);
    return item;
}

static void renderFloweristTab() {
    if (!s_consolePanel) return;
    for (int i = 0; i < 8; ++i) {
        if (s_guiPages[i]) s_guiPages[i]->setVisible(i == s_activeTab);
        if (s_navItems[i]) {
            auto* label = static_cast<cocos2d::CCLabelBMFont*>(s_navItems[i]->getLabel());
            if (label) label->setColor(i == s_activeTab ? cocos2d::ccColor3B{255, 245, 251} : cocos2d::ccColor3B{211, 181, 204});
        }
    }
    if (s_guiNavHighlight) {
        const float navY = s_guiHeight - 113.f - s_activeTab * 36.f;
        s_guiNavHighlight->setPosition({10.f, navY - 14.f});
    }
    if (s_guiTabTitle) s_guiTabTitle->setString(s_tabs[s_activeTab]);
    if (s_guiTabDescription) s_guiTabDescription->setString(s_tabDescriptions[s_activeTab]);
    if (s_guiFooter) {
        auto text = s_sim.enabled()
            ? fmt::format("BUFFER ACTIVE   /   NORMAL {}   /   WAVE {}", s_sim.normalTicks(), s_sim.waveTicks())
            : std::string("BUFFER OFF   /   F6 CYCLES PRESETS");
        s_guiFooter->setString(text.c_str());
    }
    for (int i = 0; i < 6; ++i) {
        if (s_stageCards[i]) {
            const bool selected = i == s_stageSelection;
            s_stageCards[i]->setColor(selected ? cocos2d::ccColor3B{71, 31, 64} : cocos2d::ccColor3B{38, 22, 40});
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (s_modeCards[i]) s_modeCards[i]->setColor(i == s_selectedMode ? cocos2d::ccColor3B{70, 29, 62} : cocos2d::ccColor3B{38, 22, 40});
    }
    for (int i = 0; i < 10; ++i) {
        if (s_presetCards[i]) s_presetCards[i]->setColor((s_sim.enabled() && s_sim.normalTicks() == s_presetTicks[i]) || (!s_sim.enabled() && i == 9) ? cocos2d::ccColor3B{70, 29, 62} : cocos2d::ccColor3B{38, 22, 40});
    }
    if (s_guiLog) s_guiLog->setString(s_lastLog.c_str());
    setFloweristMenusEnabled(s_consoleOpen);
}

static void createFloweristFlower() {
    if (!s_consolePanel || s_guiFlower) return;
    auto* flower = cocos2d::CCNode::create();
    if (!flower) return;

    // Six soft ring petals around a small center make a lightweight blossom watermark.
    for (int i = 0; i < 6; ++i) {
        const float angle = static_cast<float>(i) * 60.f * 3.14159265f / 180.f;
        auto* petal = cocos2d::CCLabelBMFont::create("O", "bigFont.fnt");
        if (!petal) continue;
        petal->setScale(0.31f);
        petal->setColor({255, 111, 184});
        petal->setOpacity(58);
        petal->setPosition({std::cos(angle) * 9.f, std::sin(angle) * 9.f});
        flower->addChild(petal);
    }
    auto* center = cocos2d::CCLabelBMFont::create("+", "bigFont.fnt");
    if (center) {
        center->setScale(0.22f);
        center->setColor({255, 198, 225});
        center->setOpacity(78);
        center->setPosition({0.f, 0.f});
        flower->addChild(center, 2);
    }
    flower->setPosition({s_guiWidth - 82.f, s_guiHeight - 38.f});
    flower->setScale(1.15f);
    s_consolePanel->addChild(flower, 4);
    s_guiFlower = flower;
}

static bool createFloweristConsole() {
    auto* director = cocos2d::CCDirector::sharedDirector();
    if (!director) return false;
    auto* scene = director->getRunningScene();
    if (!scene) return false;
    if (s_guiDimmer) return true;

    const auto size = director->getWinSize();
    s_guiWidth = std::min(760.f, size.width - 36.f);
    s_guiHeight = std::min(460.f, size.height - 36.f);
    // Keep the compact design on ordinary resolutions without forcing the panel off-screen.
    s_guiWidth = std::max(std::min(480.f, size.width - 24.f), s_guiWidth);
    s_guiHeight = std::max(std::min(320.f, size.height - 24.f), s_guiHeight);
    s_guiNavWidth = std::min(164.f, s_guiWidth * 0.255f);
    s_guiContentWidth = s_guiWidth - s_guiNavWidth;

    s_guiDimmer = FloweristBackdrop::create({0, 0, 0, 156}, size.width, size.height);
    if (!s_guiDimmer) return false;
    s_guiDimmer->setVisible(false);
    s_guiDimmer->setTouchEnabled(false);
    scene->addChild(s_guiDimmer, 999999);

    s_consolePanel = cocos2d::CCLayerColor::create({24, 13, 28, 255}, s_guiWidth, s_guiHeight);
    if (!s_consolePanel) {
        cleanupFloweristConsole();
        return false;
    }
    s_consolePanel->setPosition({(size.width - s_guiWidth) * 0.5f, (size.height - s_guiHeight) * 0.5f});
    s_guiDimmer->addChild(s_consolePanel, 10);

    // Main frame: crisp edge, top accent, and separated navigation rail.
    floweristRect(s_consolePanel, 0.f, 0.f, s_guiWidth, s_guiHeight, {71, 30, 61, 255}, 1);
    floweristRect(s_consolePanel, 1.f, 1.f, s_guiWidth - 2.f, s_guiHeight - 2.f, {24, 13, 28, 255}, 2);
    floweristRect(s_consolePanel, 0.f, s_guiHeight - 3.f, s_guiWidth, 3.f, {255, 83, 171, 255}, 8);
    floweristRect(s_consolePanel, 0.f, 0.f, s_guiNavWidth, s_guiHeight - 3.f, {31, 16, 34, 255}, 3);
    floweristRect(s_consolePanel, s_guiNavWidth, 14.f, 1.f, s_guiHeight - 28.f, {75, 38, 70, 255}, 4);
    floweristRect(s_consolePanel, 0.f, 0.f, s_guiNavWidth, 2.f, {255, 83, 171, 255}, 9);

    floweristLabel(s_consolePanel, "FLOWERIST", s_guiNavWidth * 0.5f, s_guiHeight - 33.f, 0.42f, {255, 112, 190}, false);
    floweristLabel(s_consolePanel, "PREDICTIVE TOOLKIT", s_guiNavWidth * 0.5f, s_guiHeight - 54.f, 0.145f, {197, 151, 184}, false, "chatFont.fnt");
    floweristRect(s_consolePanel, 17.f, s_guiHeight - 75.f, s_guiNavWidth - 34.f, 1.f, {86, 43, 77, 255});

    s_guiNavHighlight = floweristRect(s_consolePanel, 10.f, s_guiHeight - 127.f, s_guiNavWidth - 20.f, 28.f, {74, 31, 65, 255}, 4);
    s_guiActions = FloweristGuiActions::create();
    if (!s_guiActions) {
        cleanupFloweristConsole();
        return false;
    }
    s_guiActions->retain();

    s_guiNavMenu = cocos2d::CCMenu::create();
    if (!s_guiNavMenu) {
        cleanupFloweristConsole();
        return false;
    }
    s_guiNavMenu->setPosition({0.f, 0.f});
    s_guiNavMenu->setHandlerPriority(-140);
    s_consolePanel->addChild(s_guiNavMenu, 5);
    for (int i = 0; i < 8; ++i) {
        s_navItems[i] = floweristMenuLabel(s_guiNavMenu, s_guiActions, s_tabs[i], i,
            {s_guiNavWidth * 0.5f, s_guiHeight - 113.f - i * 36.f},
            menu_selector(FloweristGuiActions::tab), 0.205f,
            i == s_activeTab ? cocos2d::ccColor3B{255, 245, 251} : cocos2d::ccColor3B{211, 181, 204});
        if (s_navItems[i]) {
            const float rowW = s_guiNavWidth - 16.f;
            const float rowH = 30.f;
            // Widen the actual touch rect beyond the tiny glyphs, then re-center the label.
            s_navItems[i]->setContentSize({rowW, rowH});
            auto* navLabel = static_cast<cocos2d::CCLabelBMFont*>(s_navItems[i]->getLabel());
            if (navLabel) {
                const float naturalW = std::max(1.f, navLabel->getContentSize().width);
                const float fitScale = std::min(0.205f, (rowW - 20.f) / naturalW);
                navLabel->setScale(std::max(0.14f, fitScale));
                navLabel->setPosition({rowW * 0.5f, rowH * 0.5f});
            }
        }
    }
    floweristLabel(s_consolePanel, "F2  /  TOGGLE", s_guiNavWidth * 0.5f, 37.f, 0.16f, {191, 143, 178}, false, "chatFont.fnt");
    floweristLabel(s_consolePanel, "v1.0.0  •  LOCAL", s_guiNavWidth * 0.5f, 20.f, 0.14f, {126, 101, 124}, false, "chatFont.fnt");

    const float contentX = s_guiNavWidth + 19.f;
    floweristRect(s_consolePanel, contentX, s_guiHeight - 78.f, s_guiWidth - contentX - 18.f, 1.f, {66, 36, 64, 255});
    s_guiTabTitle = floweristLabel(s_consolePanel, s_tabs[s_activeTab], contentX, s_guiHeight - 33.f, 0.31f, {255, 130, 195}, true);
    s_guiTabDescription = floweristLabel(s_consolePanel, s_tabDescriptions[s_activeTab], contentX, s_guiHeight - 55.f, 0.205f, {195, 169, 190}, true, "chatFont.fnt");
    createFloweristFlower();

    s_closeMenu = cocos2d::CCMenu::create();
    s_closeMenu->setPosition({0.f, 0.f});
    s_closeMenu->setHandlerPriority(-140);
    s_consolePanel->addChild(s_closeMenu, 20);
    floweristRect(s_consolePanel, s_guiWidth - 42.f, s_guiHeight - 47.f, 26.f, 26.f, {65, 27, 57, 255}, 10);
    floweristMenuLabel(s_closeMenu, s_guiActions, "X", 0, {s_guiWidth - 29.f, s_guiHeight - 34.f}, menu_selector(FloweristGuiActions::close), 0.21f, {255, 170, 210});

    s_consoleStatus = floweristLabel(s_consolePanel, "READY", contentX, 15.f, 0.17f, {255, 119, 189}, true, "chatFont.fnt");
    s_guiFooter = floweristLabel(s_consolePanel, "BUFFER ACTIVE", s_guiWidth - 18.f, 15.f, 0.15f, {171, 137, 166}, false, "chatFont.fnt");

    const float pageY = 0.f;
    const float pageW = s_guiContentWidth;
    const float pageTop = s_guiHeight - 91.f;
    (void)pageY;
    (void)pageTop;
    for (int i = 0; i < 8; ++i) {
        s_guiPages[i] = cocos2d::CCLayer::create();
        if (s_guiPages[i]) {
            s_guiPages[i]->setPosition({s_guiNavWidth, 0.f});
            s_consolePanel->addChild(s_guiPages[i], 3);
        }
    }

    // OVERVIEW: 3 stat cards and a real event/history card.
    auto* overview = s_guiPages[0];
    float statW = (pageW - 54.f) / 3.f;
    floweristCard(overview, 17.f, s_guiHeight - 201.f, statW, 94.f, "BUFFER MODE", s_sim.enabled() ? "PREDICTIVE ON" : "DISABLED");
    floweristCard(overview, 27.f + statW, s_guiHeight - 201.f, statW, 94.f, "NORMAL LOOKAHEAD", fmt::format("{} TICKS", s_sim.normalTicks()), {207, 82, 154, 255});
    floweristCard(overview, 37.f + statW * 2.f, s_guiHeight - 201.f, statW, 94.f, "WAVE LOOKAHEAD", fmt::format("{} TICKS", s_sim.waveTicks()), {255, 145, 196, 255});
    auto* logCard = floweristCard(overview, 17.f, 87.f, pageW - 34.f, 126.f, "LATEST ACTIVITY", "The latest buffer and profile event appears here.", {255, 83, 171, 255});
    if (logCard) {
        s_guiLog = floweristLabel(logCard, s_lastLog, 13.f, 66.f, 0.255f, {243, 218, 235}, true, "chatFont.fnt");
        if (s_guiLog) s_guiLog->setAnchorPoint({0.f, 1.f});
        floweristLabel(logCard, "F6 CYCLES THROUGH BUFFER PRESETS", 13.f, 24.f, 0.205f, {192, 155, 183}, true, "chatFont.fnt");
    }
    floweristCard(overview, 17.f, 26.f, pageW - 34.f, 58.f, "SESSION", "GUI opens manually only. F2 works across supported game screens.", {142, 69, 125, 255});

    // STAGES: selectable profile cards in a stable 3 x 2 grid.
    auto* stagesPage = s_guiPages[1];
    s_stageMenu = cocos2d::CCMenu::create();
    s_stageMenu->setPosition({0.f, 0.f});
    s_stageMenu->setHandlerPriority(-140);
    stagesPage->addChild(s_stageMenu, 5);
    const char* stageTitles[] = {"STAGE 1", "STAGE 2", "STAGE 3", "STAGE 4", "EXTREME", "CUSTOM"};
    const char* stageBodies[] = {"2 tick profile", "8 tick profile", "16 tick profile", "24 tick profile", "48 tick profile", "Manual profile"};
    float stageCardW = (pageW - 58.f) / 3.f;
    float stageCardH = 100.f;
    for (int i = 0; i < 6; ++i) {
        float x = 17.f + (i % 3) * (stageCardW + 12.f);
        float y = s_guiHeight - 221.f - (i / 3) * 114.f;
        s_stageCards[i] = floweristCard(stagesPage, x, y, stageCardW, stageCardH, stageTitles[i], stageBodies[i], i == s_stageSelection ? cocos2d::ccColor4B{255, 83, 171, 255} : cocos2d::ccColor4B{106, 52, 91, 255});
        floweristMenuLabel(s_stageMenu, s_guiActions, i == 5 ? "SELECT" : "APPLY PROFILE", i,
            {x + stageCardW * 0.5f, y + 19.f}, menu_selector(FloweristGuiActions::stage), 0.145f, {255, 185, 218});
    }

    // MODES: selected profiles are labelled as previews; per-mode tuning is not falsely advertised.
    auto* modesPage = s_guiPages[2];
    s_modesMenu = cocos2d::CCMenu::create();
    s_modesMenu->setPosition({0.f, 0.f});
    s_modesMenu->setHandlerPriority(-140);
    modesPage->addChild(s_modesMenu, 5);
    float modeW = (pageW - 62.f) / 4.f;
    for (int i = 0; i < 8; ++i) {
        float x = 17.f + (i % 4) * (modeW + 9.f);
        float y = s_guiHeight - 212.f - (i / 4) * 118.f;
        s_modeCards[i] = floweristCard(modesPage, x, y, modeW, 98.f, s_modeNames[i], i == s_selectedMode ? "PROFILE SELECTED" : "PROFILE PREVIEW", {160, 67, 126, 255});
        floweristMenuLabel(s_modesMenu, s_guiActions, "SELECT", i,
            {x + modeW * 0.5f, y + 18.f}, menu_selector(FloweristGuiActions::mode), 0.15f, {255, 190, 218});
    }
    floweristLabel(modesPage, "Mode selection is a UI profile only; mode-specific prediction is a later phase.", 17.f, 56.f, 0.155f, {167, 133, 161}, true, "chatFont.fnt");

    // TUNING: real existing preset controls, with the current profile visibly selected.
    auto* tuningPage = s_guiPages[3];
    s_tuningMenu = cocos2d::CCMenu::create();
    s_tuningMenu->setPosition({0.f, 0.f});
    s_tuningMenu->setHandlerPriority(-140);
    tuningPage->addChild(s_tuningMenu, 5);
    float presetW = (pageW - 58.f) / 5.f;
    for (int i = 0; i < 10; ++i) {
        float x = 17.f + (i % 5) * (presetW + 6.f);
        float y = s_guiHeight - 208.f - (i / 5) * 105.f;
        s_presetCards[i] = floweristCard(tuningPage, x, y, presetW, 82.f, s_presetNames[i], i == 9 ? "BUFFER OFF" : fmt::format("{} TICKS", s_presetTicks[i]), {190, 76, 139, 255});
        floweristMenuLabel(s_tuningMenu, s_guiActions, "USE", i,
            {x + presetW * 0.5f, y + 16.f}, menu_selector(FloweristGuiActions::preset), 0.15f, {255, 186, 218});
    }
    floweristCard(tuningPage, 17.f, 68.f, pageW - 34.f, 54.f, "PREDICTION LIMIT", "Lookahead is capped at 60 ticks; candidate-delay testing is bounded.", {255, 83, 171, 255});

    // ASSISTS and OPTIMIZE explain what the current code actually does.
    auto* assistsPage = s_guiPages[4];
    floweristCard(assistsPage, 17.f, s_guiHeight - 210.f, (pageW - 45.f) / 2.f, 105.f, "INPUT BUFFER", "Predicts a safer delay before applying press/release input.");
    floweristCard(assistsPage, 26.f + (pageW - 45.f) / 2.f, s_guiHeight - 210.f, (pageW - 45.f) / 2.f, 105.f, "SHIP PASSTHROUGH", "Ship inputs bypass predictive delay in the current implementation.", {207, 82, 154, 255});
    floweristCard(assistsPage, 17.f, s_guiHeight - 329.f, (pageW - 45.f) / 2.f, 105.f, "DUAL INPUT", "The existing handler supports dual-player forwarding.", {185, 77, 137, 255});
    floweristCard(assistsPage, 26.f + (pageW - 45.f) / 2.f, s_guiHeight - 329.f, (pageW - 45.f) / 2.f, 105.f, "NOT YET ENABLED", "Straight-fly and extra assist modules are not implemented yet.", {106, 52, 91, 255});

    auto* optimizePage = s_guiPages[5];
    floweristCard(optimizePage, 17.f, s_guiHeight - 210.f, pageW - 34.f, 93.f, "BOUNDED PREDICTION", "The buffer uses capped prediction horizons and at most 16 delay candidates per input.", {255, 83, 171, 255});
    floweristCard(optimizePage, 17.f, s_guiHeight - 323.f, pageW - 34.f, 93.f, "PERFORMANCE GOAL", "Next optimization pass: cache reusable state and avoid repeated simulations when inputs have not changed.", {207, 82, 154, 255});
    floweristCard(optimizePage, 17.f, 76.f, pageW - 34.f, 48.f, "CURRENT PRIORITY", "Stability first; no FPS bypass or fake performance toggles.", {106, 52, 91, 255});

    auto* settingsPage = s_guiPages[6];
    s_settingsMenu = cocos2d::CCMenu::create();
    s_settingsMenu->setPosition({0.f, 0.f});
    s_settingsMenu->setHandlerPriority(-140);
    settingsPage->addChild(s_settingsMenu, 5);
    floweristCard(settingsPage, 17.f, s_guiHeight - 211.f, pageW - 34.f, 100.f, "REDUCED MOTION", "Disable tab entrance animation effects.", {255, 83, 171, 255});
    floweristMenuLabel(s_settingsMenu, s_guiActions, "TOGGLE", 0,
        {pageW - 72.f, s_guiHeight - 160.f}, menu_selector(FloweristGuiActions::setting), 0.17f, {255, 185, 218});
    floweristCard(settingsPage, 17.f, s_guiHeight - 329.f, pageW - 34.f, 100.f, "BUFFER HUD", "Show or hide the in-level buffer status label.", {207, 82, 154, 255});
    floweristMenuLabel(s_settingsMenu, s_guiActions, "TOGGLE", 1,
        {pageW - 72.f, s_guiHeight - 278.f}, menu_selector(FloweristGuiActions::setting), 0.17f, {255, 185, 218});
    floweristCard(settingsPage, 17.f, 68.f, pageW - 34.f, 54.f, "OVERLAY HOTKEY", "F2 opens/closes manually. Auto-open is disabled.", {106, 52, 91, 255});

    auto* competePage = s_guiPages[7];
    floweristCard(competePage, 17.f, s_guiHeight - 211.f, pageW - 34.f, 110.f, "COMMUNITY PRESETS", "Preset sharing is not connected yet. No player settings are presented as verified.", {255, 83, 171, 255});
    floweristCard(competePage, 17.f, s_guiHeight - 337.f, pageW - 34.f, 110.f, "FAIR PLAY", "Flowerist exposes its active buffer values clearly; it does not claim to bypass FPS or game rules.", {207, 82, 154, 255});

    renderFloweristTab();
    refreshFloweristConsole();
    setFloweristMenusEnabled(false);
    return true;
}

static void cleanupFloweristConsole() {
    s_consoleOpen = false;
    if (s_guiDimmer) {
        s_guiDimmer->stopAllActions();
        s_guiDimmer->removeFromParentAndCleanup(true);
    }
    s_guiDimmer = nullptr;
    s_consolePanel = nullptr;
    s_guiNavHighlight = nullptr;
    s_guiFlower = nullptr;
    s_consoleTitle = nullptr;
    s_consoleStatus = nullptr;
    s_guiTabTitle = nullptr;
    s_guiTabDescription = nullptr;
    s_guiBody = nullptr;
    s_guiLog = nullptr;
    s_guiFooter = nullptr;
    s_guiNavMenu = nullptr;
    s_closeMenu = nullptr;
    s_stageMenu = nullptr;
    s_modesMenu = nullptr;
    s_tuningMenu = nullptr;
    s_settingsMenu = nullptr;
    for (auto& item : s_navItems) item = nullptr;
    for (auto& page : s_guiPages) page = nullptr;
    for (auto& card : s_modeCards) card = nullptr;
    for (auto& card : s_stageCards) card = nullptr;
    for (auto& card : s_presetCards) card = nullptr;
    if (s_guiActions) {
        s_guiActions->release();
        s_guiActions = nullptr;
    }
}

static void refreshFloweristConsole() {
    if (s_consoleStatus) {
        auto str = s_sim.enabled()
            ? fmt::format("ACTIVE  /  NORMAL {}  /  WAVE {}", s_sim.normalTicks(), s_sim.waveTicks())
            : std::string("BUFFER OFF");
        s_consoleStatus->setString(str.c_str());
    }
    if (s_guiFooter) {
        auto text = s_sim.enabled()
            ? fmt::format("NORMAL {}  •  WAVE {}  TICKS", s_sim.normalTicks(), s_sim.waveTicks())
            : std::string("BUFFER DISABLED");
        s_guiFooter->setString(text.c_str());
    }
    renderFloweristTab();
}

static void toggleFloweristConsole() {
    if (!s_guiDimmer) {
        if (!createFloweristConsole()) return;
        s_consoleOpen = true;
        s_guiDimmer->setVisible(true);
        s_guiDimmer->setTouchEnabled(true);
        setFloweristMenusEnabled(true);
        updateFloweristFlowerAnimation();
        if (!s_reduceMotion && s_consolePanel) {
            s_consolePanel->stopAllActions();
            s_consolePanel->setScale(0.985f);
            s_consolePanel->runAction(cocos2d::CCScaleTo::create(0.16f, 1.f));
        }
        return;
    }
    s_consoleOpen = !s_consoleOpen;
    s_guiDimmer->stopAllActions();
    if (s_consolePanel) {
        s_consolePanel->stopAllActions();
        s_consolePanel->setScale(1.f);
    }
    s_guiDimmer->setVisible(s_consoleOpen);
    s_guiDimmer->setTouchEnabled(s_consoleOpen);
    setFloweristMenusEnabled(s_consoleOpen);
    updateFloweristFlowerAnimation();
    if (s_consoleOpen && s_consolePanel && !s_reduceMotion) {
        s_consolePanel->setScale(0.985f);
        s_consolePanel->runAction(cocos2d::CCScaleTo::create(0.16f, 1.f));
    }
    refreshFloweristConsole();
}

static void refreshBufferHud() {
    if (!s_bufferHud) return;

    if (!s_sim.enabled()) {
        s_bufferHud->setString("BUFFER: OFF");
        s_bufferHud->setColor({255, 130, 130});
    } else {
        auto normal = s_sim.normalTicks();
        auto wave = s_sim.waveTicks();

        auto status = normal == wave
            ? fmt::format("BUFFER: {} TICKS", normal)
            : fmt::format("BUFFER: {}/{} TICKS", normal, wave);

        s_bufferHud->setString(status.c_str());
        s_bufferHud->setColor({255, 105, 180});
    }
}

// ============================================================
// FLOWERIST V2 uses an in-game GUI; no external Windows console.

// ============================================================
// PLAY LAYER
// ============================================================

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority(
            "PlayLayer::destroyPlayer", -1000000
        );
        (void)self.setHookPriority(
            "PlayLayer::playEndAnimationToPos", -1000000
        );
        (void)self.setHookPriority(
            "PlayLayer::flipGravity", -1000000
        );
    }

    bool init(
        GJGameLevel* level,
        bool useReplay,
        bool dontCreateObjects
    ) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        s_sim.init();

        auto size =
            cocos2d::CCDirector::sharedDirector()->getWinSize();

        s_bufferHud = cocos2d::CCLabelBMFont::create(
            "BUFFER: 2 TICKS", "bigFont.fnt"
        );

        if (s_bufferHud) {
            s_bufferHud->setScale(0.35f);
            s_bufferHud->setAnchorPoint({0.f, 1.f});
            s_bufferHud->setPosition({8.f, size.height - 8.f});
            s_bufferHud->setZOrder(9999);
            this->addChild(s_bufferHud);
            s_bufferHud->setVisible(s_showBufferHud);
            refreshBufferHud();
        }

        // Intentionally do not create/open Flowerist here. GUI is manual-only via F2.
        return true;
    }

    void destroyPlayer(
        PlayerObject* player,
        GameObject* gameObject
    ) {
        if (s_sim.checkSimulationDeath(player)) return;
        PlayLayer::destroyPlayer(player, gameObject);
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(
        PlayerObject* p0,
        bool p1,
        bool p2
    ) {
        if (s_sim.isSimulating()) return;
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void onQuit() {
        // Remove simulation children and overlay references before the layer is torn down.
        s_sim.quit();
        cleanupFloweristConsole();
        s_bufferHud = nullptr;
        PlayLayer::onQuit();
    }
};
// ============================================================
// LEVEL EDITOR
// ============================================================

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool unk) {
        bool res = LevelEditorLayer::init(level, unk);
        s_sim.quit();
        return res;
    }
};

// ============================================================
// HARD STREAK
// ============================================================

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority(
            "HardStreak::addPoint", -1000000
        );
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

// ============================================================
// EFFECT GAME OBJECT
// ============================================================

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority(
            "EffectGameObject::triggerObject", -1000000
        );
    }

    void triggerObject(
        GJBaseGameLayer* p0,
        int p1,
        const gd::vector<int>* p2
    ) {
        if (s_sim.isSimulating()) return;
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

// ============================================================
// GAME OBJECT
// ============================================================

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority(
            "GameObject::playShineEffect", -1000000
        );
    }

    void playShineEffect() {
        if (s_sim.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

// ============================================================
// PLAYER OBJECT
// ============================================================

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority(
            "PlayerObject::playSpiderDashEffect", -1000000
        );
        (void)self.setHookPriority(
            "PlayerObject::incrementJumps", -1000000
        );
        (void)self.setHookPriority(
            "PlayerObject::ringJump", -1000000
        );
    }

    void playSpiderDashEffect(
        cocos2d::CCPoint from,
        cocos2d::CCPoint to
    ) {
        if (s_sim.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (s_sim.isSimulating()) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        if (!s_sim.isSimulating()) {
            s_sim.onPlayerUpdate(this);
        }

        PlayerObject::update(dt);

        if (PlayLayer::get() && !s_sim.isSimulating()) {
            s_sim.setFrameDelta(dt);
        }
    }

    void ringJump(RingObject* p0, bool p1) {
        if (s_sim.isSimulating()) return;
        PlayerObject::ringJump(p0, p1);
    }
};

// ============================================================
// BASE GAME LAYER
// ============================================================

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority(
            "GJBaseGameLayer::canBeActivatedByPlayer", -1000000
        );
        (void)self.setHookPriority(
            "GJBaseGameLayer::handleButton", -1000000
        );
        (void)self.setHookPriority(
            "GJBaseGameLayer::collisionCheckObjects", -1000000
        );
        (void)self.setHookPriority(
            "GJBaseGameLayer::playerTouchedRing", -1000000
        );
    }

    bool canBeActivatedByPlayer(
        PlayerObject* p0,
        EffectGameObject* p1
    ) {
        if (s_sim.isSimulating()) return false;

        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!s_sim.isSimulating() &&
            !s_sim.isFiringBuffered() &&
            button == 1 &&
            PlayLayer::get()) {

            if (!s_sim.interceptInput(this, down, isPlayer1)) {
                return;
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(
        PlayerObject* player,
        gd::vector<GameObject*>* vec,
        int objectsCount,
        float dt
    ) {
        if (s_sim.isSimulating()) {
            gd::vector<GameObject*> extra;
            extra.reserve(objectsCount);

            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;

                if (obj->m_objectType == GameObjectType::Solid ||
                    obj->m_objectType == GameObjectType::Hazard ||
                    obj->m_objectType == GameObjectType::AnimatedHazard ||
                    obj->m_objectType == GameObjectType::CollisionObject ||
                    obj->m_objectType == GameObjectType::Slope) {
                    extra.push_back(obj);
                }
            }

            GJBaseGameLayer::collisionCheckObjects(
                player, &extra, static_cast<int>(extra.size()), dt
            );
            return;
        }

        GJBaseGameLayer::collisionCheckObjects(
            player, vec, objectsCount, dt
        );
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (s_sim.isSimulating()) return;

        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};



// ============================================================
// GLOBAL F2 — works on menu, gameplay, and pause overlays
// ============================================================

class $modify(BufKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (key == KEY_F2 && isKeyDown && !isKeyRepeat) {
            toggleFloweristConsole();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

// Drop scene-bound GUI references before Cocos destroys the old running scene.
class $modify(BufDirector, CCDirector) {
    void willSwitchToScene(CCScene* scene) {
        if (s_guiDimmer) cleanupFloweristConsole();
        CCDirector::willSwitchToScene(scene);
    }
};

