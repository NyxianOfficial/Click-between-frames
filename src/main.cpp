
#include "includes.hpp"
#include "cocos2d.h" 

#include <limits>
#include <chrono>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/CreatorLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>

constexpr double SMALLEST_FLOAT = std::numeric_limits<float>::min();

// Corregir inicialización de InputEvent
constexpr InputEvent EMPTY_INPUT = InputEvent{ 0, InputState::Press, false };
constexpr Step EMPTY_STEP = Step{ EMPTY_INPUT, 1.0, true };

// Declarar inputQueueCopy como extern
extern std::queue<struct InputEvent> inputQueueCopy;

std::queue<struct Step> stepQueue;

std::atomic<bool> softToggle;

InputEvent nextInput = EMPTY_INPUT;

uint64_t lastFrameTime = 0;
uint64_t currentFrameTime = 0;

bool firstFrame = true; // necessary to prevent accidental inputs at the start of the level or when unpausing
bool skipUpdate = true; // true -> dont split steps during PlayerObject::update()
bool enableInput = false;
bool lateCutoff; // false -> ignore inputs that happen after the start of the frame; true -> check for inputs at the latest possible moment

/*
this function copies over the inputQueue from the input thread and uses it to build a queue of physics steps
based on when each input happened relative to the start of the frame
(and also calculates the associated stepDelta multipliers for each step)
*/
void buildStepQueue(int stepCount) {
    PlayLayer* playLayer = PlayLayer::get();
    nextInput = EMPTY_INPUT;
    stepQueue = {}; // shouldnt be necessary, but just in case

    currentFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    std::lock_guard<std::mutex> lock(inputQueueLock);
    inputQueueCopy = inputQueue;
    inputQueue = {};

    if (!firstFrame) skipUpdate = false;
    else {
        skipUpdate = true;
        firstFrame = false;
        lastFrameTime = currentFrameTime;
        inputQueueCopy = {};
        return;
    }

    uint64_t deltaTime = currentFrameTime - lastFrameTime;
    uint64_t stepDelta = (deltaTime / stepCount) + 1; // the +1 is to prevent dropped inputs caused by integer division

    for (int i = 0; i < stepCount; i++) { // for each physics step of the frame
        double elapsedTime = 0.0;
        while (!inputQueueCopy.empty()) { // while loop to account for multiple inputs on the same step
            InputEvent front = inputQueueCopy.front();

            if (front.time <= lastFrameTime + stepDelta * (i + 1)) { // if the first input in the queue happened on the current step
                double inputTime = static_cast<double>((front.time - lastFrameTime) % stepDelta) / stepDelta; // proportion of step elapsed at the time the input was made
                stepQueue.emplace(Step{ front, std::clamp(inputTime - elapsedTime, SMALLEST_FLOAT, 1.0), false });
                inputQueueCopy.pop();
                elapsedTime = inputTime;
            }
            else break; // no more inputs this step, more later in the frame
        }

        stepQueue.emplace(Step{ EMPTY_INPUT, std::max(SMALLEST_FLOAT, 1.0 - elapsedTime), true });
    }

    lastFrameTime = currentFrameTime;
}

/*
return the first step in the queue,
also check if an input happened on the previous step, if so run handleButton.
tbh this doesnt need to be a separate function from the PlayerObject::update() hook
*/
Step popStepQueue() {
    if (stepQueue.empty()) return EMPTY_STEP;

    Step front = stepQueue.front();
    double deltaFactor = front.deltaFactor;

    if (nextInput.time != 0) {
        PlayLayer* playLayer = PlayLayer::get();

        enableInput = true;
        playLayer->handleButton(nextInput.inputState == InputState::Press, 0, nextInput.isPlayer1); // Corregir parámetros
        enableInput = false;
    }

    nextInput = front.input;
    stepQueue.pop();

    return front;
}

/*
"decompiled" version of PlayerObject::resetCollisionLog() since it's inlined in GD 2.2074 on Windows
*/
void decomp_resetCollisionLog(PlayerObject* p) {
    (*(CCDictionary**)((char*)p + 0x5b0))->removeAllObjects();
    (*(CCDictionary**)((char*)p + 0x5b8))->removeAllObjects();
    (*(CCDictionary**)((char*)p + 0x5c0))->removeAllObjects();
    (*(CCDictionary**)((char*)p + 0x5c8))->removeAllObjects();
    *(unsigned long*)((char*)p + 0x5e0) = *(unsigned long*)((char*)p + 0x5d0);
    *(long long*)((char*)p + 0x5d0) = -1;
}

double averageDelta = 0.0;

bool legacyBypass;
bool actualDelta;

/*
determine the number of physics steps that happen on each frame, 
need to rewrite the vanilla formula bc otherwise you'd have to use inline assembly to get the step count
*/
int calculateStepCount(float delta, float timewarp, bool forceVanilla) {
    if (!actualDelta || forceVanilla) { // vanilla 2.2
        return std::round(std::max(1.0, ((delta * 60.0) / std::min(1.0f, timewarp)) * 4.0)); // not sure if this is different from `(delta * 240) / timewarp` bc of float precision
    }
    else if (legacyBypass) { // 2.1 physics bypass (same as vanilla 2.1)
        return std::round(std::max(4.0, delta * 240.0) / std::min(1.0f, timewarp));
    }
    else { // bypass physics with support for higher frequencies like 360 Hz
        double animationInterval = CCDirector::sharedDirector()->getAnimationInterval();
        averageDelta = (0.05 * delta) + (0.95 * averageDelta); // exponential moving average to detect lag/external fps caps
        if (averageDelta > animationInterval * 10) averageDelta = animationInterval * 10; // dont let averageDelta get too high
        
        bool laggingOneFrame = animationInterval < delta - (1.0 / 360.0); // more than 1 step of lag on a single frame
        bool laggingManyFrames = averageDelta - animationInterval > 0.0005; // average lag is >0.5ms
        
        if (!laggingOneFrame && !laggingManyFrames) { // no stepcount variance when not lagging
            return std::round(std::ceil((animationInterval * 360.0) - 0.0001) / std::min(1.0f, timewarp));
        } 
        else if (!laggingOneFrame) { // consistently low fps
            return std::round(std::ceil(averageDelta * 360.0) / std::min(1.0f, timewarp));
        }
        else { // need to catch up badly
            return std::round(std::ceil(delta * 360.0) / std::min(1.0f, timewarp));
        }
    }
}

bool safeMode;

class $modify(PlayLayer) {
    // update keybinds when you enter a level
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    // disable progress in safe mode
    void levelComplete() {
        bool testMode = this->m_isTestMode;
        if (safeMode && !softToggle.load()) this->m_isTestMode = true;

        PlayLayer::levelComplete();

        this->m_isTestMode = testMode;
    }

    // disable new best popup in safe mode
    void showNewBest(bool p0, int p1, int p2, bool p3, bool p4, bool p5) {
        if (!safeMode || softToggle.load()) PlayLayer::showNewBest(p0, p1, p2, p3, p4, p5);
    }
};

bool mouseFix;

class $modify(CCEGLView) {
    void pollEvents() {
        PlayLayer* playLayer = PlayLayer::get();
        CCNode* par;

        currentFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();

        if (softToggle.load() // CBF disabled
            || !playLayer // not in level
            || !(par = playLayer->getParent()) // must be a real playLayer with a parent (for compatibility with mods that use a fake playLayer)
            || (par->getChildByType<PauseLayer>(0)) // if paused
            || (playLayer->getChildByType<EndLevelLayer>(0))) // if on endscreen
        {
            firstFrame = true;
            skipUpdate = true;
            enableInput = true;

            inputQueueCopy = {};

            std::lock_guard<std::mutex> lock(inputQueueLock);
            inputQueue = {};
        }
        if (mouseFix && !skipUpdate) { // reduce lag with high polling rate mice by limiting the number of mouse movements per frame to 1
            // Handle mouse fix logic here
        }

        // Reemplazar la llamada a pollEvents con una implementación vacía o personalizada
        // Si no es necesario, simplemente no hagas nada aquí.
    }
};

// Reemplazar UINT32 con uint32_t
uint32_t stepCount;

class $modify(GJBaseGameLayer) {
    static void onModify(auto& self) {
        // Manejar advertencias de [[nodiscard]] ignorando el valor explícitamente
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
        (void)self.setHookPriority("GJBaseGameLayer::getModifiedDelta", Priority::VeryEarly);
    }

    // disable regular inputs while CBF is active
    void handleButton(bool down, int button, bool isPlayer1) {
        if (enableInput) GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // either use the modified delta to calculate the step count, or use the actual delta if physics bypass is enabled
    float getModifiedDelta(float delta) {
        float modifiedDelta = GJBaseGameLayer::getModifiedDelta(delta);

        PlayLayer* pl = PlayLayer::get();
        if (pl) {
            const float timewarp = pl->m_gameState.m_timeWarp;
            if (actualDelta) modifiedDelta = CCDirector::sharedDirector()->getActualDeltaTime() * timewarp;
            
            stepCount = calculateStepCount(modifiedDelta, timewarp, false);

            if (pl->m_player1->m_isDead || GameManager::sharedState()->getEditorLayer()) {
                enableInput = true;
                skipUpdate = true;
                firstFrame = true;
            }
            else if (modifiedDelta > 0.0) buildStepQueue(stepCount);
            else skipUpdate = true;
        }
        else if (actualDelta) stepCount = calculateStepCount(modifiedDelta, this->m_gameState.m_timeWarp, true); // disable physics bypass outside levels
        
        return modifiedDelta;
    }
};

// Reemplazar NULL con 0.0f para CCPoint
CCPoint p1Pos = { 0.0f, 0.0f };
CCPoint p2Pos = { 0.0f, 0.0f };

float rotationDelta;
bool midStep = false;

class $modify(PlayerObject) {
    // split a single step based on the entries in stepQueue
    void update(float stepDelta) {
        PlayLayer* pl = PlayLayer::get();

        if (skipUpdate 
            || !pl 
            || !(this == pl->m_player1 || this == pl->m_player2)) // for compatibility with mods like Globed
        {
            PlayerObject::update(stepDelta);
            return;
        }

        PlayerObject* p2 = pl->m_player2;
        if (this == p2) return; // do all of the logic during the P1 update for simplicity

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

        p1Pos = PlayerObject::getPosition(); // save for later to prevent desync with move triggers & some other issues
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
                    if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) this->m_isOnGround = p1StartedOnGround; // this fixes delayed inputs on platforms moving down for some reason
                    if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true); // moving platforms will launch u really high if this is anything other than 0.0, idk why
                    else pl->checkCollisions(this, stepDelta, true); // slopes will launch you really high if the 2nd argument is lower than like 0.01, idk why
                    PlayerObject::updateRotation(substepDelta);
                    decomp_resetCollisionLog(this); // presumably this function clears the list of objects that the icon is touching, necessary for wave
                }
            }
            else if (step.endStep) { // revert to click-on-steps mode when buffering to reduce bugs
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
                }
                else if (step.endStep) {
                    p2->update(stepDelta);
                }
            }

            firstLoop = false;
        } while (!step.endStep);

        midStep = false;
    }

    // this function was chosen to update m_lastPosition in just because it's called right at the end of the vanilla physics step loop
    void updateRotation(float t) {
        PlayLayer* pl = PlayLayer::get();
        if (!skipUpdate && pl && this == pl->m_player1) {
            PlayerObject::updateRotation(rotationDelta); // perform the remaining rotation that was left incomplete in the PlayerObject::update() hook

            if (p1Pos.x && !midStep) { // ==true only at the end of a step that an input happened on
                this->m_lastPosition = p1Pos; // move triggers & spider get confused without this (iirc)
                p1Pos.setPoint(0.0f, 0.0f);
            }
        }
        else if (!skipUpdate && pl && this == pl->m_player2) {
            PlayerObject::updateRotation(rotationDelta);

            if (p2Pos.x && !midStep) {
                this->m_lastPosition = p2Pos;
                p2Pos.setPoint(0.0f, 0.0f);
            }
        }
        else PlayerObject::updateRotation(t);

        if (actualDelta && pl && !midStep) {
            pl->m_gameState.m_currentProgress = static_cast<int>(pl->m_gameState.m_levelTime * 240.0);
        }
    }
};

/*
CBF/PB endscreen watermark
*/
class $modify(EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();

        if (!softToggle.load() || actualDelta) {
            std::string text;

            if (softToggle.load() && actualDelta) text = "PB";
            else if (actualDelta) text = "CBF+PB";
            else text = "CBF";

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

/*
notify the player if theres an issue with input on Linux
*/
class $modify(CreatorLayer) {
    bool init() {
        if (!CreatorLayer::init()) return false;

        // Handle Linux input failure notification here

        return true;
    } 
};

/*
dont submit to leaderboards for rated levels
*/
class $modify(GJGameLevel) {
    void savePercentage(int percent, bool p1, int clicks, int attempts, bool valid) {
        valid = (
            Mod::get()->getSettingValue<bool>("soft-toggle")
            && !Mod::get()->getSettingValue<bool>("actual-delta")
            || this->m_stars == 0
        );

        if (!safeMode || softToggle.load()) GJGameLevel::savePercentage(percent, p1, clicks, attempts, valid);
    }
};

Patch* pbPatch;

void togglePhysicsBypass(bool enable) {
    actualDelta = enable;
}

Patch* modPatch;

void toggleMod(bool disable) {
    softToggle.store(disable);
}

// Declarar inputThread como extern
extern void inputThread();

$on_mod(Loaded) {
    Mod::get()->setSavedValue<bool>("is-linux", false);

    toggleMod(Mod::get()->getSettingValue<bool>("soft-toggle"));
    listenForSettingChanges("soft-toggle", toggleMod);

    togglePhysicsBypass(Mod::get()->getSettingValue<bool>("actual-delta"));
    listenForSettingChanges("actual-delta", togglePhysicsBypass);

    legacyBypass = Mod::get()->getSettingValue<std::string>("bypass-mode") == "2.1";
    listenForSettingChanges("bypass-mode", +[](std::string mode) {
        legacyBypass = mode == "2.1";
    });

    safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
    listenForSettingChanges("safe-mode", +[](bool enable) {
        safeMode = enable;
    });

    mouseFix = Mod::get()->getSettingValue<bool>("mouse-fix");
    listenForSettingChanges("mouse-fix", +[](bool enable) {
        mouseFix = enable;
    });

    lateCutoff = Mod::get()->getSettingValue<bool>("late-cutoff");
    listenForSettingChanges("late-cutoff", +[](bool enable) {
        lateCutoff = enable;
    });

    threadPriority = Mod::get()->getSettingValue<bool>("thread-priority");

    std::thread(inputThread).detach();
}
