#include "includes.hpp"

#include <limits>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/CreatorLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <chrono>
#include <cstdint>
#include <queue>
#include <atomic>

constexpr double SMALLEST_FLOAT = std::numeric_limits<float>::min();
constexpr InputEvent EMPTY_INPUT = InputEvent{ std::chrono::steady_clock::time_point{std::chrono::seconds(0)}, PlayerButton::Jump, false, true };
constexpr Step EMPTY_STEP = Step{ EMPTY_INPUT, 1.0, true };

std::queue<InputEvent> inputQueueCopy;
std::queue<Step> stepQueue;

std::atomic<bool> softToggle; // Mantener softToggle para desactivar el mod
std::atomic<bool> touchFix; // Declarar touchFix

InputEvent nextInput = EMPTY_INPUT;

std::chrono::steady_clock::time_point lastFrameTime;
std::chrono::steady_clock::time_point currentFrameTime;

bool firstFrame = true;
bool skipUpdate = true;
bool linuxNative = false;
bool lateCutoff;

void buildStepQueue(int stepCount) {
    PlayLayer* playLayer = PlayLayer::get();
    nextInput = EMPTY_INPUT;
    stepQueue = {};

    currentFrameTime = std::chrono::steady_clock::now();
    if (linuxNative) {
        linuxCheckInputs();
    }

    std::lock_guard lock(inputQueueLock);
    inputQueueCopy = inputQueue;
    inputQueue = {};

    if (!firstFrame) skipUpdate = false;
    else {
        skipUpdate = true;
        firstFrame = false;
        lastFrameTime = currentFrameTime;
        return;
    }

    auto deltaTime = std::chrono::duration_cast<std::chrono::microseconds>(currentFrameTime - lastFrameTime).count();
    auto stepDelta = (deltaTime / stepCount) + 1;

    for (int i = 0; i < stepCount; i++) {
        double elapsedTime = 0.0;
        while (!inputQueueCopy.empty()) {
            InputEvent front = inputQueueCopy.front();
            if (front.time.time_since_epoch().count() - lastFrameTime.time_since_epoch().count() < stepDelta * (i + 1)) {
                double inputTime = static_cast<double>((front.time.time_since_epoch().count() - lastFrameTime.time_since_epoch().count()) % stepDelta) / stepDelta;
                stepQueue.emplace(Step{ front, std::clamp(inputTime - elapsedTime, SMALLEST_FLOAT, 1.0), false });
                inputQueueCopy.pop();
                elapsedTime = inputTime;
            } else break;
        }

        stepQueue.emplace(Step{ EMPTY_INPUT, std::max(SMALLEST_FLOAT, 1.0 - elapsedTime), true });
    }

    lastFrameTime = currentFrameTime;
}

Step popStepQueue() {
    if (stepQueue.empty()) return EMPTY_STEP;

    Step front = stepQueue.front();
    double deltaFactor = front.deltaFactor;

    if (nextInput.time.time_since_epoch().count() != 0) {
        PlayLayer* playLayer = PlayLayer::get();
        playLayer->handleButton(nextInput.inputState, static_cast<int>(nextInput.inputType), nextInput.isPlayer1);
    }

    nextInput = front.input;
    stepQueue.pop();

    return front;
}

void decomp_resetCollisionLog(PlayerObject* p) {
    (*(CCDictionary**)((char*)p + 0x5b0))->removeAllObjects();
    (*(CCDictionary**)((char*)p + 0x5b8))->removeAllObjects();
    (*(CCDictionary**)((char*)p + 0x5c0))->removeAllObjects();
    (*(CCDictionary**)((char*)p + 0x5c8))->removeAllObjects();
    *(uint64_t*)((char*)p + 0x5e0) = *(uint64_t*)((char*)p + 0x5d0);
    *(uint64_t*)((char*)p + 0x5d0) = -1;
}

double averageDelta = 0.0;
bool legacyBypass;
bool actualDelta;

int calculateStepCount(float delta, float timewarp, bool forceVanilla) {
    if (!actualDelta || forceVanilla) {
        return std::round(std::max(1.0, ((delta * 240.0) / std::min(1.0f, timewarp)) * 4.0));
    } else if (legacyBypass) {
        return std::round(std::max(4.0, delta * 240.0) / std::min(1.0f, timewarp));
    } else {
        double animationInterval = cocos2d::CCDirector::sharedDirector()->getAnimationInterval();
        averageDelta = (0.05 * delta) + (0.95 * averageDelta);
        if (averageDelta > animationInterval * 10) averageDelta = animationInterval * 10;

        bool laggingOneFrame = animationInterval < delta - (1.0 / 360.0);
        bool laggingManyFrames = averageDelta - animationInterval > 0.0005;

        if (!laggingOneFrame && !laggingManyFrames) {
            return std::round(std::ceil((animationInterval * 360.0) - 0.0001) / std::min(1.0f, timewarp));
        } else if (!laggingOneFrame) {
            return std::round(std::ceil(averageDelta * 360.0) / std::min(1.0f, timewarp));
        } else {
            return std::round(std::ceil(delta * 360.0) / std::min(1.0f, timewarp));
        }
    }
}

bool safeMode;

class $modify(PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void levelComplete() {
        bool testMode = this->m_isTestMode;
        if (safeMode && !softToggle.load()) this->m_isTestMode = true;

        PlayLayer::levelComplete();
        this->m_isTestMode = testMode;
    }

    void showNewBest(bool p0, int p1, int p2, bool p3, bool p4, bool p5) {
        if (!safeMode || softToggle.load()) PlayLayer::showNewBest(p0, p1, p2, p3, p4, p5);
    }
};

bool mouseFix;

class $modify(GJBaseGameLayer) {
    static void onModify(auto& self) {
        // Manejar el valor de retorno para evitar advertencias
        auto result1 = self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
        auto result2 = self.setHookPriority("GJBaseGameLayer::getModifiedDelta", Priority::VeryEarly);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    float getModifiedDelta(float delta) {
        float modifiedDelta = GJBaseGameLayer::getModifiedDelta(delta);

        PlayLayer* pl = PlayLayer::get();
        if (pl) {
            const float timewarp = pl->m_gameState.m_timeWarp;
            if (actualDelta) modifiedDelta = cocos2d::CCDirector::sharedDirector()->getActualDeltaTime() * timewarp;

            int stepCount = calculateStepCount(modifiedDelta, timewarp, false);

            if (pl->m_player1->m_isDead || GameManager::sharedState()->getEditorLayer()) {
                skipUpdate = true;
                firstFrame = true;
            } else if (modifiedDelta > 0.0) buildStepQueue(stepCount);
            else skipUpdate = true;
        } else if (actualDelta) {
            int stepCount = calculateStepCount(modifiedDelta, this->m_gameState.m_timeWarp, true);
        }

        return modifiedDelta;
    }
};

CCPoint p1Pos = { 0.0f, 0.0f };
CCPoint p2Pos = { 0.0f, 0.0f };

float rotationDelta;
bool midStep = false;

class $modify(PlayerObject) {
    void update(float stepDelta) {
        PlayLayer* pl = PlayLayer::get();

        if (skipUpdate || !pl || !(this == pl->m_player1 || this == pl->m_player2)) {
            PlayerObject::update(stepDelta);
            return;
        }

        PlayerObject* p2 = pl->m_player2;
        if (this == p2) return;

        bool isDual = pl->m_gameState.m_isDualMode;

        bool p1StartedOnGround = this->m_isOnGround;
        bool p2StartedOnGround = p2->m_isOnGround;

        bool p1NotBuffering = p1StartedOnGround || this->m_touchingRings->count() || this->m_isDashing || (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);
        bool p2NotBuffering = p2StartedOnGround || p2->m_touchingRings->count() || p2->m_isDashing || (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing);

        p1Pos = PlayerObject::getPosition();
        p2Pos = p2->getPosition();

        Step step;
        bool firstLoop = true;
        midStep = true;

        do {
            step = popStepQueue();

            const float substepDelta = stepDelta * step.deltaFactor;
            rotationDelta = substepDelta;

            if (p1NotBuffering) {
                PlayerObject::update(substepDelta);
                if (!step.endStep) {
                    if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) this->m_isOnGround = p1StartedOnGround;
                    if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true);
                    else pl->checkCollisions(this, stepDelta, true);
                    PlayerObject::updateRotation(substepDelta);
                    decomp_resetCollisionLog(this);
                }
            } else if (step.endStep) {
                PlayerObject::update(stepDelta);
            }

            if (isDual) {
                if (p2NotBuffering) {
                    p2->update(substepDelta);
                    if (!step.endStep) {
                        if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
                        if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                        else pl->checkCollisions(p2, stepDelta, true);
                        p2->updateRotation(substepDelta);
                        decomp_resetCollisionLog(p2);
                    }
                } else if (step.endStep) {
                    p2->update(stepDelta);
                }
            }

            firstLoop = false;
        } while (!step.endStep);

        midStep = false;
    }

    void updateRotation(float t) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && this == pl->m_player1) {
            PlayerObject::updateRotation(rotationDelta);

            if (p1Pos.x) {
                this->m_lastPosition = p1Pos;
                p1Pos.setPoint(0.0f, 0.0f);
            }
        } else if (pl && this == pl->m_player2) {
            PlayerObject::updateRotation(rotationDelta);

            if (p2Pos.x) {
                this->m_lastPosition = p2Pos;
                p2Pos.setPoint(0.0f, 0.0f);
            }
        } else PlayerObject::updateRotation(t);

        if (actualDelta && pl && !midStep) {
            pl->m_gameState.m_currentProgress = static_cast<int>(pl->m_gameState.m_levelTime * 240.0);
        }
    }
};

class $modify(EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();

        if (actualDelta) {
            std::string text = "CBF";

            cocos2d::CCSize size = cocos2d::CCDirector::sharedDirector()->getWinSize();
            CCLabelBMFont* indicator = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");

            indicator->setPosition({ size.width, size.height });
            indicator->setAnchorPoint({ 1.0f, 1.0f });
            indicator->setOpacity(30);
            indicator->setScale(0.2f);

            this->addChild(indicator);
        }
    }
};

class $modify(CreatorLayer) {
    bool init() {
        if (!CreatorLayer::init()) return false;

        return true;
    } 
};

class $modify(GJGameLevel) {
    void savePercentage(int percent, bool p1, int clicks, int attempts, bool valid) {
        valid = (this->m_stars == 0);

        if (!safeMode) GJGameLevel::savePercentage(percent, p1, clicks, attempts, valid);
    }
};

$on_mod(Loaded) {
    legacyBypass = Mod::get()->getSettingValue<std::string>("bypass-mode") == "2.1";
    geode::listenForSettingChanges("bypass-mode", +[](std::string mode) {
        legacyBypass = mode == "2.1";
    });

    safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
    geode::listenForSettingChanges("safe-mode", +[](bool enable) {
        safeMode = enable;
    });

    touchFix = Mod::get()->getSettingValue<bool>("touch-fix");
    geode::listenForSettingChanges("touch-fix", +[](bool enable) {
        touchFix = enable;
    });

    lateCutoff = Mod::get()->getSettingValue<bool>("late-cutoff");
    geode::listenForSettingChanges("late-cutoff", +[](bool enable) {
        lateCutoff = enable;
    });
    
    threadPriority = Mod::get()->getSettingValue<bool>("thread-priority");
    geode::listenForSettingChanges("thread-priority", +[](bool enable) {
        threadPriority = enable;
    });

    softToggle = Mod::get()->getSettingValue<bool>("soft-toggle");
    geode::listenForSettingChanges("soft-toggle", +[](bool enable) {
        softToggle = enable;
    });
}
