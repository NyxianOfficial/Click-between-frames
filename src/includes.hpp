#pragma once

#include <algorithm>
#include <queue>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <cstdint> // Para tipos estándar como uint64_t
#include <Geode/Geode.hpp>

using namespace geode::prelude;

// Enumeraciones para acciones de juego y estados de entrada
enum class GameAction : int {
    p1Jump = 0,
    p1Left = 1,
    p1Right = 2,
    p2Jump = 3,
    p2Left = 4,
    p2Right = 5
};

enum class InputState : int {
    Press = 0,
    Release = 1
};

// Estructuras para eventos de entrada
struct InputEvent {
    uint32_t time;
    InputState inputState;
    bool isPlayer1;

    InputEvent& operator=(const InputEvent& other) {
        if (this != &other) {
            time = other.time;
            inputState = other.inputState;
            isPlayer1 = other.isPlayer1;
        }
        return *this;
    }
};

struct Step {
    InputEvent input;
    double deltaFactor;
    bool endStep;
};

// Variables globales
extern std::queue<InputEvent> inputQueue;
extern std::queue<InputEvent> inputQueueCopy;
extern std::unordered_set<uint16_t> heldInputs; // Reemplazo de USHORT
extern std::mutex inputQueueLock;
extern std::atomic<bool> enableRightClick;
extern std::atomic<bool> softToggle;
extern bool threadPriority;

constexpr size_t BUFFER_SIZE = 20; // Tamaño del buffer

void inputThread();
