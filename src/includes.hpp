#pragma once

#include <algorithm>
#include <queue>
#include <mutex>
#include <array>
#include <unordered_set>
#include <atomic>
#include <chrono>

#include <Geode/Geode.hpp>

using namespace geode::prelude;

enum GameAction : int {
    p1Jump = 0,
    p1Left = 1,
    p1Right = 2,
    p2Jump = 3,
    p2Left = 4,
    p2Right = 5
};

enum State : bool {
    Release = 0,
    Press = 1
};

struct __attribute__((packed)) LinuxInputEvent {
    std::chrono::steady_clock::time_point time;
    uint16_t type;
    uint16_t code;
    int value;
};

struct InputEvent {
    std::chrono::steady_clock::time_point time;
    PlayerButton inputType;
    bool inputState;
    bool isPlayer1;
};

struct Step {
    InputEvent input;
    double deltaFactor;
    bool endStep;
};

// Declaraciones de variables usando extern 
extern std::queue<InputEvent> inputQueue;
extern std::queue<InputEvent> inputQueueCopy; // Asegúrate de que esté aquí
extern std::array<std::unordered_set<size_t>, 6> inputBinds;
extern std::unordered_set<uint16_t> heldInputs;

extern std::mutex inputQueueLock;
extern std::mutex keybindsLock;

extern std::atomic<bool> enableRightClick;
extern std::atomic<bool> softToggle; 

extern bool threadPriority;

constexpr size_t BUFFER_SIZE = 20;

void inputThread();
