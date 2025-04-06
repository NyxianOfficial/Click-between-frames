#include "includes.hpp"
#include <Geode/modify/CCDirector.hpp>
#include <chrono>
#include <thread>

// Definiciones de las variables
std::queue<InputEvent> inputQueue;
std::array<std::unordered_set<size_t>, 6> inputBinds;
std::unordered_set<uint16_t> heldInputs;

std::mutex inputQueueLock;
std::mutex keybindsLock;

std::atomic<bool> enableRightClick{false};
bool threadPriority = false;

void inputThread() {
    // Implementación del input thread por alguna razon puse 3 milisegundos jaja
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3)); // Simular retraso de fotogramas
    }
}
