#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Windows.h>
#include <cstdio>
#include <algorithm>

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
        if (!pl || !pl->m_objectLayer) return nullptr;

        PlayerObject* player =
            PlayerObject::create(1, 1, pl, pl, true);

        if (!player) return nullptr;

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
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;

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

        // F2 toggles the in-game Flowerist GUI.
        if (GetAsyncKeyState(VK_F2) & 1) {
            toggleFloweristConsole();
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
// FLOWERIST GLOBALS
// ============================================================

static TrajectoryBufferSim s_sim;
static cocos2d::CCLabelBMFont* s_bufferHud = nullptr;

static bool s_consoleOpen = false;
static cocos2d::CCLayerColor* s_consolePanel = nullptr;
static cocos2d::CCLabelBMFont* s_consoleTitle = nullptr;
static cocos2d::CCLabelBMFont* s_consoleStatus = nullptr;
static cocos2d::CCLabelBMFont* s_consoleHint = nullptr;
static cocos2d::CCMenu* s_guiMenu = nullptr;
static cocos2d::CCLabelBMFont* s_guiBody = nullptr;
static cocos2d::CCLabelBMFont* s_guiLog = nullptr;
static cocos2d::CCLayerColor* s_guiAccent = nullptr;
static cocos2d::CCLabelBMFont* s_guiTabTitle = nullptr;
static int s_activeTab = 1;
static int s_stageSelection = 0;
static bool s_reduceMotion = false;
static const char* s_tabs[] = {"LOGS", "STAGES", "MODES", "TUNING", "ASSISTS", "OPTIMIZE", "SETTINGS", "COMPETE"};
static std::string s_lastLog = "Flowerist V2 ready";
static void renderFloweristTab();
static void appendFloweristLog(const std::string& event) {
    s_lastLog = event;
    if (s_activeTab == 0) renderFloweristTab();
}
class FloweristGuiActions : public cocos2d::CCObject {
public:
    static FloweristGuiActions* create() {
        auto obj = new FloweristGuiActions();
        obj->autorelease();
        return obj;
    }
    void tab(cocos2d::CCObject* sender) {
        s_activeTab = static_cast<cocos2d::CCNode*>(sender)->getTag();
        if (s_guiBody && !s_reduceMotion) {
            s_guiBody->stopAllActions();
            s_guiBody->setOpacity(110);
            s_guiBody->setPositionX(124.f);
            s_guiBody->runAction(cocos2d::CCSpawn::create(
                cocos2d::CCFadeTo::create(0.14f, 255),
                cocos2d::CCEaseBackOut::create(cocos2d::CCMoveTo::create(0.24f, {129.f, s_guiBody->getPositionY()})), nullptr));
        }
        renderFloweristTab();
    }
    void stage(cocos2d::CCObject* sender) {
        const int selected = static_cast<cocos2d::CCNode*>(sender)->getTag();
        s_stageSelection = selected;
        // Stage 1-4 and Extreme map to conservative existing buffer presets.
        const int preset[] = {0, 2, 4, 6, 8};
        if (selected < 5) s_sim.selectPreset(preset[selected]);
        appendFloweristLog(fmt::format("Selected {}", selected == 5 ? "CUSTOM (configure later)" : fmt::format("STAGE {}", selected + 1)));
        refreshBufferHud();
        refreshFloweristConsole();
        renderFloweristTab();
    }
    void motion(cocos2d::CCObject*) {
        s_reduceMotion = !s_reduceMotion;
        renderFloweristTab();
    }
};
static FloweristGuiActions* s_guiActions = nullptr;
static cocos2d::CCMenu* s_stageMenu = nullptr;
static cocos2d::CCMenu* s_modesMenu = nullptr;
static std::vector<cocos2d::CCNode*> s_modeTiles;
static void renderFloweristTab() {
    if (!s_guiBody) return;
    if (s_stageMenu) s_stageMenu->setVisible(s_activeTab == 1);
    for (auto* tile : s_modeTiles) if (tile) tile->setVisible(s_activeTab == 2);
    if (s_guiTabTitle) s_guiTabTitle->setString(s_tabs[s_activeTab]);
    if (s_guiAccent) s_guiAccent->setPositionY(155.f - s_activeTab * 18.f);
    const char* pages[] = {
        "LATEST EVENT:\n\n",
        "CHOOSE A BUFFER STAGE\n\nLEARN FROM ATTEMPTS\nComing in a future build",
        "SELECT A GAMEMODE BELOW\nMINI VARIANTS: PLANNED\nPER-MODE TUNING: PLANNED",
        "MIN / MAX TICKS: planned\nCLICK & RELEASE: planned\nAUTO TUNE: planned\n60 TICK MAXIMUM",
        "AUTO STRAIGHT FLY: planned\nLEGIT STRAIGHT FLY: planned",
        "SHOW FPS / FPS BYPASS\nNO SHADERS / PARTICLES / GLOW\nAUTO LDM: planned",
        "F2 - OPEN / CLOSE\nF6 - CYCLE PRESETS\nSPRING ANIMATIONS: ON\nREDUCED MOTION: planned",
        "TOP 5 PLAYER PRESETS\nAwaiting verified settings"
    };
    auto content = std::string(pages[s_activeTab]);
    if (s_activeTab == 0) content += s_lastLog;
    s_guiBody->setString(content.c_str());
}
static void createFloweristConsole(PlayLayer* layer) {
    if (!layer || s_consolePanel) return;
    const auto size = cocos2d::CCDirector::sharedDirector()->getWinSize();
    const float w = std::min(370.f, size.width - 24.f);
    const float h = std::min(225.f, size.height - 24.f);
    s_consolePanel = cocos2d::CCLayerColor::create({54,16,38,255},w,h);
    if (!s_consolePanel) return;
    s_consolePanel->setPosition({(size.width-w)*0.5f,(size.height-h)*0.5f});
    s_consolePanel->setZOrder(10000);
    s_consolePanel->setVisible(false);
    // Keep GUI lifetime tied to this level, not the outgoing running scene.
    layer->addChild(s_consolePanel, 999999);

    auto side = cocos2d::CCLayerColor::create({92,29,65,255},113.f,h);
    s_consolePanel->addChild(side);
    auto border = cocos2d::CCLayerColor::create({255,105,180,255},w,2.f);
    border->setPosition({0,h-2.f});
    s_consolePanel->addChild(border);
    auto divider = cocos2d::CCLayerColor::create({193,75,130,255},1.f,h-18.f);
    divider->setPosition({113.f,8.f});
    s_consolePanel->addChild(divider);

    s_consoleTitle = cocos2d::CCLabelBMFont::create("FLOWERIST", "bigFont.fnt");
    if (s_consoleTitle) {
        s_consoleTitle->setColor({255,105,180});
        s_consoleTitle->setScale(0.38f);
        s_consoleTitle->setPosition({56.f,h-21.f});
        s_consolePanel->addChild(s_consoleTitle);
    }
    auto subtitle = cocos2d::CCLabelBMFont::create("PREDICTIVE TOOLKIT", "bigFont.fnt");
    subtitle->setScale(0.14f);
    subtitle->setColor({255,175,209});
    subtitle->setPosition({56.f,h-39.f});
    s_consolePanel->addChild(subtitle);

    s_guiActions = FloweristGuiActions::create();
    s_guiActions->retain();
    s_guiMenu = cocos2d::CCMenu::create();
    s_guiMenu->setPosition({0,0});
    s_consolePanel->addChild(s_guiMenu);
    s_guiAccent = cocos2d::CCLayerColor::create({255,105,180,255},3.f,15.f);
    s_guiAccent->setPosition({4.f,155.f});
    s_consolePanel->addChild(s_guiAccent);
    for (int i=0;i<8;++i) {
        auto label = cocos2d::CCLabelBMFont::create(s_tabs[i],"bigFont.fnt");
        label->setScale(0.235f);
        label->setColor({255,202,225});
        auto item = cocos2d::CCMenuItemLabel::create(label,s_guiActions,menu_selector(FloweristGuiActions::tab));
        item->setTag(i);
        item->setPosition({55.f,162.f-i*18.f});
        s_guiMenu->addChild(item);
    }
    s_guiTabTitle = cocos2d::CCLabelBMFont::create("STAGES","bigFont.fnt");
    s_guiTabTitle->setColor({255,105,180});
    s_guiTabTitle->setScale(0.32f);
    s_guiTabTitle->setAnchorPoint({0.f,0.5f});
    s_guiTabTitle->setPosition({129.f,h-22.f});
    s_consolePanel->addChild(s_guiTabTitle);
    s_guiBody = cocos2d::CCLabelBMFont::create("", "bigFont.fnt");
    s_guiBody->setScale(0.205f);
    s_guiBody->setColor({255,215,232});
    s_guiBody->setAnchorPoint({0.f,1.f});
    s_guiBody->setPosition({129.f,h-51.f});
    s_consolePanel->addChild(s_guiBody);
    s_stageMenu = cocos2d::CCMenu::create();
    s_stageMenu->setPosition({0,0});
    s_consolePanel->addChild(s_stageMenu);
    const char* names[] = {"1","2","3","4","EXTREME","CUSTOM"};
    for(int i=0;i<6;++i) {
        auto label=cocos2d::CCLabelBMFont::create(names[i],"bigFont.fnt");
        label->setScale(i<4?0.27f:0.17f);
        label->setColor({255,105,180});
        auto item=cocos2d::CCMenuItemLabel::create(label,s_guiActions,menu_selector(FloweristGuiActions::stage));
        item->setTag(i);
        item->setPosition({150.f+(i%3)*67.f,77.f-(i/3)*23.f});
        s_stageMenu->addChild(item);
    }
    s_modesMenu = cocos2d::CCMenu::create();
    s_modesMenu->setPosition({0,0});
    s_consolePanel->addChild(s_modesMenu);
    const char* modeNames[] = {"CUBE","SHIP","BALL","UFO","WAVE","ROBOT","SPIDER","SWING"};
    for (int i=0; i<8; ++i) {
        auto tile = cocos2d::CCLayerColor::create({130,39,89,255},51.f,30.f);
        tile->setPosition({128.f+(i%4)*57.f,66.f-(i/4)*37.f});
        s_consolePanel->addChild(tile);
        auto label=cocos2d::CCLabelBMFont::create(modeNames[i],"bigFont.fnt");
        label->setScale(0.17f);
        label->setColor({255,211,231});
        label->setPosition({25.5f,15.f});
        tile->addChild(label);
        // Tiles remain noninteractive until per-mode buffering is implemented.
        // They are shown/hidden with the Modes tab below.
        s_modeTiles.push_back(tile);
    }
    s_consoleStatus = cocos2d::CCLabelBMFont::create("BUFFER", "bigFont.fnt");
    s_consoleStatus->setScale(0.19f);
    s_consoleStatus->setColor({255,105,180});
    s_consoleStatus->setPosition({(w+113.f)/2.f,14.f});
    s_consolePanel->addChild(s_consoleStatus);

    // Lightweight decorative sparkles: no particle emitter or per-frame update.
    for (int i=0;i<7;++i) {
        auto star=cocos2d::CCLabelBMFont::create("+","bigFont.fnt");
        star->setScale(0.15f+(i%3)*0.045f);
        star->setColor({255,145,205});
        star->setPosition({122.f+(i*29)%std::max(1, static_cast<int>(w-132.f)),h-10.f-(i%3)*5.f});
        star->setOpacity(85);
        s_consolePanel->addChild(star);
        star->runAction(cocos2d::CCRepeatForever::create(cocos2d::CCSequence::create(
            cocos2d::CCFadeTo::create(0.7f+0.12f*i,180),
            cocos2d::CCFadeTo::create(0.8f+0.1f*i,55),nullptr)));
    }
    renderFloweristTab();
    refreshFloweristConsole();
}
static void refreshFloweristConsole() {
    if (!s_consoleStatus) return;
    auto str = s_sim.enabled()
        ? fmt::format("NORMAL {} | WAVE {} TICKS",s_sim.normalTicks(),s_sim.waveTicks())
        : std::string("BUFFER OFF");
    s_consoleStatus->setString(str.c_str());
}
// Call while the level is still alive. The GUI pointers below are non-owning.
static void cleanupFloweristConsole() {
    s_consoleOpen = false;
    if (s_consolePanel) {
        s_consolePanel->stopAllActions();
        s_consolePanel->removeFromParentAndCleanup(true);
    }
    s_consolePanel = nullptr;
    s_consoleTitle = nullptr;
    s_consoleStatus = nullptr;
    s_consoleHint = nullptr;
    s_guiMenu = nullptr;
    s_guiBody = nullptr;
    s_guiLog = nullptr;
    s_guiAccent = nullptr;
    s_guiTabTitle = nullptr;
    s_stageMenu = nullptr;
    s_modesMenu = nullptr;
    s_modeTiles.clear();
    if (s_guiActions) {
        s_guiActions->release();
        s_guiActions = nullptr;
    }
}

static void toggleFloweristConsole() {
    if (!s_consolePanel) return;

    s_consolePanel->stopAllActions();
    s_consoleOpen = !s_consoleOpen;
    s_consolePanel->setScale(1.0f);
    s_consolePanel->setVisible(s_consoleOpen);

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
            refreshBufferHud();
        }

        createFloweristConsole(this);
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
        // Release child objects before base teardown invalidates the level.
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


