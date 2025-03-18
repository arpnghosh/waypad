#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

#define THRESHOLD 10

class Gamepad {
public:
  Gamepad(const string &path);
  ~Gamepad();
  void updateState();
  bool isAnyButtonPressed() const;
  bool isAxisMoved() const;
  bool isAnyTriggerPressed() const;

  string path;
  unique_ptr<libevdev, void (*)(libevdev *)> evdev;
  vector<bool> buttons;
  vector<float> axes;
  vector<float> triggers;
};

Gamepad::Gamepad(const string &path)
    : evdev(nullptr, [](libevdev *dev) { libevdev_free(dev); }) {
  int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd == -1) {
    throw runtime_error("Failed to open device: " + path);
  }
  libevdev *dev = nullptr;

  int rc = libevdev_new_from_fd(fd, &dev);
  if (rc < 0) {
    close(fd);
    throw runtime_error("Failed to initialize libevdev");
  }
  buttons.push_back(false);
  triggers.push_back(0.0f);
  axes.push_back(0.0f);

  evdev = unique_ptr<libevdev, void (*)(libevdev *)>(
      dev, [](libevdev *dev) { libevdev_free(dev); });
}

Gamepad::~Gamepad() {}

void Gamepad::updateState() {
  struct input_event ev;
  while (libevdev_next_event(evdev.get(), LIBEVDEV_READ_FLAG_NORMAL, &ev) ==
         LIBEVDEV_READ_STATUS_SUCCESS) {
    switch (ev.type) {
    case EV_KEY:
      if (ev.code >= BTN_A && ev.code <= BTN_THUMBR) {
        cout << "button input" << endl;
        buttons[ev.code - BTN_A] = ev.value != 0;
      }
      break;

    case EV_ABS:
      switch (ev.code) {
      case ABS_X:
      case ABS_Y:
      case ABS_RX:
      case ABS_RY: {
        int maxValue = libevdev_get_abs_maximum(evdev.get(), ev.code);
        int minValue = libevdev_get_abs_minimum(evdev.get(), ev.code);
        if (maxValue > minValue) {
          float range = static_cast<float>(maxValue - minValue);
          cout << "axis movement" << endl;
          axes[ev.code - ABS_X] = (ev.value - minValue) / range * 2.0f - 1.0f;
        } else {
          axes[ev.code - ABS_X] = 0.0f;
        }
        break;
      }

      case ABS_Z:
      case ABS_RZ: {
        int maxValue = libevdev_get_abs_maximum(evdev.get(), ev.code);
        cout << "trigger input" << endl;
        triggers[ev.code - ABS_Z] =
            maxValue > 0 ? ev.value / static_cast<float>(maxValue) : 0.0f;
      } break;

      default:
        break;
      }
    }
  }
}

bool Gamepad::isAnyButtonPressed() const {
  for (bool button : buttons) {
    if (button) {
      return true;
    }
  }
  return false;
}

bool Gamepad::isAxisMoved() const {
  for (float axis : axes) {
    if (axis) {
      return true;
    }
  }
  return false;
}

bool Gamepad::isAnyTriggerPressed() const {
  const float threshold = 0.0f;
  for (float trigger : triggers) {
    if (trigger > threshold) {
      return true;
    }
  }
  return false;
}

filesystem::path findDevice(const filesystem::path &inputDeviceFolder) {
  for (const auto &entry : filesystem::directory_iterator(inputDeviceFolder)) {
    if (filesystem::is_symlink(entry.path())) {
      filesystem::path canonicalPath = filesystem::canonical(entry.path());
      if (canonicalPath.string().find("-event-joystick")) {
        return canonicalPath;
      }
    }
  }
  return filesystem::path();
}

int main() {
  try {
    filesystem::path deviceEventFile = findDevice("/dev/input/by-id/");
    if (deviceEventFile.empty()) {
      cout << "Game controller is not connected" << endl;
      return EXIT_FAILURE;
    }

    Gamepad gamepad(deviceEventFile);

    auto lastActiveTime = chrono::steady_clock::now();

    while (true) {
      bool active = false;
      gamepad.updateState();

      bool anyButtonPressed = gamepad.isAnyButtonPressed();
      if (anyButtonPressed) {
        active = true;
        lastActiveTime = chrono::steady_clock::now();
      }
      if (!active) {
        bool axisMoved = gamepad.isAxisMoved();
        if (axisMoved) {
          active = true;
          lastActiveTime = chrono::steady_clock::now();
        }
      }
      if (!active) {
        bool triggerPress = gamepad.isAnyTriggerPressed();
        if (triggerPress) {
          active = true;
          lastActiveTime = chrono::steady_clock::now();
        }
      }

      auto currentTime = chrono::steady_clock::now();
      auto inactiveDuration =
          chrono::duration_cast<chrono::seconds>(currentTime - lastActiveTime);
      if (inactiveDuration.count() >= 10) {
        cout << "Inactive controller" << endl;
      }

      this_thread::sleep_for(chrono::milliseconds(THRESHOLD));
    }
  } catch (const exception &e) {
    cerr << "Error: " << e.what() << endl;
    return EXIT_FAILURE;
  }
  return 0;
}
