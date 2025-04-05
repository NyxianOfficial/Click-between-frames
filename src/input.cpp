#include "includes.hpp"

#include <mutex>
#include <unordered_set>
#include <queue>
#include <array>
#include <cstdint>
#include <chrono> // Para manejar el tiempo
#include <android/input.h> // Asegúrate de incluir la cabecera de Android

std::queue<InputEvent> inputQueue;
std::queue<InputEvent> inputQueueCopy;
std::array<std::unordered_set<size_t>, 6> inputBinds;
std::unordered_set<uint16_t> heldInputs;

std::mutex inputQueueLock;
std::atomic<bool> enableRightClick;
bool threadPriority;
