#include "includes.hpp"

#include <mutex>
#include <unordered_set>
#include <queue>
#include <array>
#include <cstdint>
#include <chrono> // Para manejar el tiempo
#include <thread> // Para manejar hilos
#include <atomic>
#include <android/input.h> // Asegúrate de incluir la cabecera de Android

std::queue<InputEvent> inputQueue;
std::queue<InputEvent> inputQueueCopy; // Definir inputQueueCopy aquí
std::array<std::unordered_set<size_t>, 6> inputBinds;
std::unordered_set<uint16_t> heldInputs;

std::mutex inputQueueLock;
std::atomic<bool> enableRightClick;
bool threadPriority;

// Implementar inputThread
void inputThread() {
    while (true) {
        // Simular procesamiento de eventos de entrada
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // Simular 60 FPS
    }
}
