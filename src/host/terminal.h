#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include <termios.h>

namespace kestrel {

// Puts stdin in raw mode and forwards keystrokes to the guest's serial port.
// Ctrl-] requests shutdown. Restores the terminal on destruction.
class Terminal {
 public:
  using InputFn = std::function<void(uint8_t)>;
  using QuitFn = std::function<void()>;

  Terminal(InputFn on_input, QuitFn on_quit);
  ~Terminal();

  static void write_output(uint8_t byte);

 private:
  void reader_loop();

  InputFn on_input_;
  QuitFn on_quit_;
  bool raw_ = false;
  termios saved_{};
  std::atomic<bool> stop_{false};
  std::thread reader_;
};

}  // namespace kestrel
