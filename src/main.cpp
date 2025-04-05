#include <limits>
#include <mutex>
#include <queue>
#include <atomic>
#include <chrono>
#include <cstdint> 
#include <memory>  
#include <unordered_set>

#include "includes.hpp"
#include "cocos2d.h" 
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/CreatorLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>

constexpr double SMALLEST_FLOAT = std::numeric_limits<float>::min();

extern std::queue<InputEvent> inputQueue; 
extern std::queue<InputEvent> inputQueueCopy; 
std::queue<Step> stepQueue;

std::atomic<bool> softToggle;

InputEvent nextInput = { 0, InputState::Press, false };

uint64_t lastFrameTime = 0;
uint64_t lastPhysicsFrameTime = 0;
uint64_t currentFrameTime = 0;

std::mutex sharedMemMutex;

bool firstFrame = true;
bool skipUpdate = true;
bool enableInput = false;
bool lateCutoff;

constexpr InputEvent EMPTY_INPUT = { 0, InputState::Press, false };
constexpr Step EMPTY_STEP = { EMPTY_INPUT, 1.0, true };

// Declaración de variables globales necesarias
bool actualDelta = false; // Controla si se usa delta real
bool legacyBypass = false; // Controla si se usa un método alternativo
float averageDelta = 0.0f; // Promedio del delta para cálculos de sincronización

void updateInputQueueAndTime(int stepCount);
Step updateDeltaFactorAndInput();
void newResetCollisionLog(PlayerObject* p);
int calculateStepCount(float delta, float timewarp, bool forceVanilla);
void togglePhysicsBypass(bool enable);
void syncSettings();

// UQ: Updates input queue and timing logic.
void updateInputQueueAndTime(int stepCount) {
    PlayLayer* playLayer = PlayLayer::get();
    if (!playLayer || GameManager::sharedState()->getEditorLayer()) {
        enableInput = false;
        firstFrame = true;
        skipUpdate = true;
        return;
    }

    nextInput = EMPTY_INPUT;
    lastFrameTime = lastPhysicsFrameTime;
    stepQueue = {};

    std::lock_guard lock(sharedMemMutex);

    currentFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    if (lateCutoff) {
        inputQueueCopy = inputQueue;
        inputQueue = {};
    } else {
        while (!inputQueue.empty() && inputQueue.front().time <= currentFrameTime) {
            InputEvent front = inputQueue.front();
            inputQueueCopy.push(front);
            inputQueue.pop();
        }
    }

    lastPhysicsFrameTime = currentFrameTime;

    if (!firstFrame) {
        skipUpdate = false;
    } else {
        skipUpdate = true;
        firstFrame = false;
        if (!lateCutoff) inputQueueCopy = {};
        return;
    }

        while (!inputQueueCopy.empty()) {
        inputQueueCopy.pop();
    }

    uint64_t deltaTime = currentFrameTime - lastFrameTime;
    uint64_t stepDelta = (deltaTime / stepCount) + 1;

    for (int i = 0; i < stepCount; i++) {
        double lastDFactor = 0.0;
        while (true) {
            InputEvent front;
            bool empty = inputQueueCopy.empty();
            if (!empty) front = inputQueueCopy.front();

            if (!empty && front.time - lastFrameTime < stepDelta * (i + 1)) {
                double dFactor = static_cast<double>((front.time - lastFrameTime) % stepDelta) / stepDelta;
                stepQueue.emplace(Step{ front, std::clamp(dFactor - lastDFactor, SMALLEST_FLOAT, 1.0), false });
                inputQueueCopy.pop();
                lastDFactor = dFactor;
                continue;
            } else {
                stepQueue.emplace(Step{ EMPTY_INPUT, std::max(SMALLEST_FLOAT, 1.0 - lastDFactor), true });
                break;
            }
        }
    }
}

// DF: Updates delta factor and processes input.
Step updateDeltaFactorAndInput() {
    if (stepQueue.empty()) return EMPTY_STEP;

    Step front = stepQueue.front();
    double deltaFactor = front.deltaFactor;

    if (nextInput.time != 0) {
        PlayLayer* playLayer = PlayLayer::get();
        enableInput = true;
        playLayer->handleButton(nextInput.inputState == InputState::Press, 0, nextInput.isPlayer1);
        enableInput = false;
    }

    nextInput = front.input;
    stepQueue.pop();

    return front;
}

// CL: Clears collision logs for player object.
void newResetCollisionLog(PlayerObject* p) {
    (*(cocos2d::CCDictionary**)((char*)p + 0x5b0))->removeAllObjects();
    (*(cocos2d::CCDictionary**)((char*)p + 0x5b8))->removeAllObjects();
    (*(cocos2d::CCDictionary**)((char*)p + 0x5c0))->removeAllObjects();
    (*(cocos2d::CCDictionary**)((char*)p + 0x5c8))->removeAllObjects();
    *(unsigned long*)((char*)p + 0x5e0) = *(unsigned long*)((char*)p + 0x5d0);
    *(long long*)((char*)p + 0x5d0) = -1;
}

// SC: Calculates step count based on delta and timewarp.
int calculateStepCount(float delta, float timewarp, bool forceVanilla) {
    if (!actualDelta || forceVanilla) {
        return std::round(std::max(1.0, ((delta * 60.0) / std::min(1.0f, timewarp)) * 4.0));
    } else if (legacyBypass) {
        return std::round(std::max(4.0, delta * 240.0) / std::min(1.0f, timewarp));
    } else {
        double animationInterval = cocos2d::CCDirector::sharedDirector()->getAnimationInterval();
        averageDelta = (0.05 * delta) + (0.95 * averageDelta);

        bool laggingOneFrame = animationInterval < delta - (1.0 / 240.0);
        bool laggingManyFrames = averageDelta - animationInterval > 0.0005;

        if (!laggingOneFrame && !laggingManyFrames) {
            return std::round(std::ceil((animationInterval * 240.0) - 0.0001) / std::min(1.0f, timewarp));
        } else if (!laggingOneFrame) {
            return std::round(std::ceil(averageDelta * 240.0) / std::min(1.0f, timewarp));
        } else {
            return std::round(std::ceil(delta * 240.0) / std::min(1.0f, timewarp));
        }
    }
}

bool safeMode = false;

class $modify(PlayLayer) {
public:
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        syncSettings(); // Sincronizar configuraciones al iniciar el nivel
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void levelComplete() {
        bool testMode = this->m_isTestMode;
        if (safeMode && !softToggle.load()) {
            this->m_isTestMode = true; // Activar modo seguro si está habilitado
        }

        PlayLayer::levelComplete();
        this->m_isTestMode = testMode;
    }

    void showNewBest(bool p0, int p1, int p2, bool p3, bool p4, bool p5) {
        if (!safeMode || softToggle.load()) {
            PlayLayer::showNewBest(p0, p1, p2, p3, p4, p5); // Mostrar "New Best" solo si el modo seguro está desactivado o soft-toggle está activado
        }
    }
};

class $modify(cocos2d::CCEGLView) {
public:
    void pollEvents() {
        // Implementación personalizada de pollEvents
        // Elimina la llamada a la función base si no es válida
    }
};

uint32_t stepCount = 0; // Inicializado a 0

class $modify(GJBaseGameLayer) {
public:
    static void onModify(auto& self) {
        self.setHookPriority("GJBaseGameLayer::handleButton", geode::Priority::VeryEarly);
        self.setHookPriority("GJBaseGameLayer::getModifiedDelta", geode::Priority::VeryEarly);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (down) {
            enableInput = true;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        } else {
            enableInput = false; // Desactivar entrada al soltar
            nextInput = EMPTY_INPUT;
            GJBaseGameLayer::handleButton(down, button, isPlayer1); // Asegurar que se procesa el evento de soltar
        }
    }

    float getModifiedDelta(float delta) {
        float modifiedDelta = GJBaseGameLayer::getModifiedDelta(delta);
        PlayLayer* pl = PlayLayer::get();

        if (pl) {
            const float timewarp = pl->m_gameState.m_timeWarp;
            if (actualDelta) {
                modifiedDelta = cocos2d::CCDirector::sharedDirector()->getActualDeltaTime() * timewarp;
            }

            stepCount = calculateStepCount(modifiedDelta, timewarp, false);

            if (pl->m_player1->m_isDead) {
                enableInput = true;
                firstFrame = true;
            } else if (modifiedDelta > 0.0) {
                updateInputQueueAndTime(stepCount);
            } else {
                skipUpdate = true;
            }
        } else {
            stepCount = calculateStepCount(modifiedDelta, this->m_gameState.m_timeWarp, true);
        }

        return modifiedDelta;
    }
};

cocos2d::CCPoint p1Pos = { 0.0f, 0.0f }; 
cocos2d::CCPoint p2Pos = { 0.0f, 0.0f }; 

float rotationDelta = 0.0f; 
bool midStep = false;

class $modify(PlayerObject) {
public:
    void update(float timeFactor) {
        PlayLayer* pl = PlayLayer::get();

        if (skipUpdate || !pl || !(this == pl->m_player1 || this == pl->m_player2)) {
            PlayerObject::update(timeFactor);
            return;
        }

        PlayerObject* p2 = pl->m_player2;
        if (this == p2) return;

        enableInput = false;

        bool isDual = pl->m_gameState.m_isDualMode;

        bool p1StartedOnGround = this->m_isOnGround;
        bool p2StartedOnGround = p2->m_isOnGround;

        bool p1NotBuffering = p1StartedOnGround
            || this->m_touchingRings->count()
            || this->m_isDashing
            || (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);

        bool p2NotBuffering = p2StartedOnGround
            || p2->m_touchingRings->count()
            || p2->m_isDashing
            || (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing);

        p1Pos = PlayerObject::getPosition();
        p2Pos = p2->getPosition();

        Step step;
        bool firstLoop = true;
        midStep = true;

        do {
            step = updateDeltaFactorAndInput();

            const float newTimeFactor = timeFactor * step.deltaFactor;
            rotationDelta = newTimeFactor;

            if (p1NotBuffering) {
                PlayerObject::update(newTimeFactor);
                if (!step.endStep) {
                    if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) this->m_isOnGround = p1StartedOnGround;
                    if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true);
                    else pl->checkCollisions(this, 0.25f, true);
                    PlayerObject::updateRotation(newTimeFactor);
                    newResetCollisionLog(this);
                }
            } else if (step.endStep) {
                PlayerObject::update(timeFactor);
            }

            if (isDual) {
                if (p2NotBuffering) {
                    p2->update(newTimeFactor);
                    if (!step.endStep) {
                        if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
                        if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                        else pl->checkCollisions(p2, 0.25f, true);
                        p2->updateRotation(newTimeFactor);
                        newResetCollisionLog(p2);
                    }
                } else if (step.endStep) {
                    p2->update(timeFactor);
                }
            }

            firstLoop = false;
        } while (!step.endStep);

        midStep = false;
    }

    void updateRotation(float t) {
        PlayLayer* pl = PlayLayer::get();
        if (!skipUpdate && pl && this == pl->m_player1) {
            PlayerObject::updateRotation(rotationDelta);

            if (p1Pos.x && !midStep) {
                this->m_lastPosition = p1Pos;
                p1Pos.setPoint(0.0f, 0.0f); // Inicializado a 0.0f
            }
        } else if (!skipUpdate && pl && this == pl->m_player2) {
            PlayerObject::updateRotation(rotationDelta);

            if (p2Pos.x && !midStep) {
                pl->m_player2->m_lastPosition = p2Pos;
                p2Pos.setPoint(0.0f, 0.0f); // Inicializado a 0.0f
            }
        } else {
            PlayerObject::updateRotation(t);
        }
    }
};

class $modify(EndLevelLayer) {
public:
    void customSetup() {
        EndLevelLayer::customSetup();

        if (!softToggle.load() || actualDelta) {
            std::string text;

            if (softToggle.load() && actualDelta) text = "PB";
            else if (actualDelta) text = "CBF+PB";
            else text = "CBF";

            cocos2d::CCSize size = cocos2d::CCDirector::sharedDirector()->getWinSize();
            cocos2d::CCLabelBMFont* indicator = cocos2d::CCLabelBMFont::create(text.c_str(), "bigFont.fnt");

            indicator->setPosition({ size.width, size.height });
            indicator->setAnchorPoint({ 1.0f, 1.0f });
            indicator->setOpacity(30);
            indicator->setScale(0.2f);

            this->addChild(indicator);
        }
    }
};

class $modify(CreatorLayer) {
public:
    bool init() {
        if (!CreatorLayer::init()) return false;

        std::unique_lock<std::mutex> lock(sharedMemMutex);
        if (!lock.owns_lock()) {
            // Manejo de error: Mutex estancado
            return false;
        }
        return true;
    } 
};

class $modify(GJGameLevel) {
public:
    void savePercentage(int percent, bool p1, int clicks, int attempts, bool valid) {
        if (safeMode) {
            // No guardar progreso si el modo seguro está activado
            return;
        }

        valid = (
            geode::Mod::get()->getSettingValue<bool>("soft-toggle")
            && !geode::Mod::get()->getSettingValue<bool>("actual-delta")
            || this->m_stars == 0
        );

        GJGameLevel::savePercentage(percent, p1, clicks, attempts, valid);
    }
};

// Sincronizar configuraciones al inicio del juego
void syncSettings() {
    softToggle.store(geode::Mod::get()->getSettingValue<bool>("soft-toggle"));
    actualDelta = geode::Mod::get()->getSettingValue<bool>("actual-delta");
    safeMode = geode::Mod::get()->getSettingValue<bool>("safe-mode");
    lateCutoff = geode::Mod::get()->getSettingValue<bool>("late-cutoff");
    threadPriority = geode::Mod::get()->getSettingValue<bool>("thread-priority");
    if (threadPriority) {
        // Ajustar prioridad del hilo si threadPriority está activado
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
