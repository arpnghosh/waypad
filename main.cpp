#include "include/idle-inhibit-unstable-v1-client-protocol.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
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
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

using namespace std;

#define THRESHOLD 10

struct wlContext {
  struct wl_display *display = nullptr;
  struct wl_compositor *compositor = nullptr;
  struct wl_surface *surface = nullptr;
  struct zwp_idle_inhibit_manager_v1 *idle_inhibit_manager = nullptr;
  struct zwp_idle_inhibitor_v1 *idle_inhibitor = nullptr;
};

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

  buttons.resize(BTN_THUMBR - BTN_A + 1, false);
  axes.resize(ABS_RY - ABS_X + 1, 0.0f);
  triggers.resize(ABS_RZ - ABS_Z + 1, 0.0f);

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
        /*cout << "button input" << endl;*/
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
          /*cout << "axis movement" << endl;*/
          axes[ev.code - ABS_X] = (ev.value - minValue) / range * 2.0f - 1.0f;
        } else {
          axes[ev.code - ABS_X] = 0.0f;
        }
        break;
      }

      case ABS_Z:
      case ABS_RZ: {
        int maxValue = libevdev_get_abs_maximum(evdev.get(), ev.code);
        /*cout << "trigger input" << endl;*/
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
  const float threshold = 0.1f;
  for (float axis : axes) {
    if (axis > threshold) {
      return true;
    }
  }
  return false;
}

bool Gamepad::isAnyTriggerPressed() const {
  const float threshold = 0.1f;
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

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  wlContext *context = static_cast<wlContext *>(data);
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    context->compositor = static_cast<wl_compositor *>(
        wl_registry_bind(registry, name, &wl_compositor_interface, version));
  } else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) ==
             0) {
    context->idle_inhibit_manager =
        static_cast<zwp_idle_inhibit_manager_v1 *>(wl_registry_bind(
            registry, name, &zwp_idle_inhibit_manager_v1_interface, version));
  }
}

static const struct wl_registry_listener registryListener = {
    .global = registry_handle_global,
    .global_remove = [](void *, struct wl_registry *, uint32_t) {}};
;

bool connectToWayland(wlContext &context) {

  context.display = wl_display_connect(nullptr);
  if (!context.display) {
    cerr << "Failed to connect to wayland display " << endl;
    return false;
  }

  struct wl_registry *registry = wl_display_get_registry(context.display);
  if (!registry) {
    cerr << "Failed to get wayland registry" << endl;
    wl_display_disconnect(context.display);
    return false;
  }

  wl_registry_add_listener(registry, &registryListener, &context);
  wl_display_roundtrip(context.display);

  if (!context.compositor || !context.idle_inhibit_manager) {
    cerr << "Required Wayland globals not available" << endl;
    wl_display_disconnect(context.display);
    return false;
  }

  context.surface = wl_compositor_create_surface(context.compositor);
  if (!context.surface) {
    cerr << "Failed to create Wayland surface" << endl;
    wl_display_disconnect(context.display);
    return false;
  }
  wl_surface_commit(context.surface);

  return true;
}

void clean(wlContext &context) {
  zwp_idle_inhibitor_v1_destroy(context.idle_inhibitor);
  wl_surface_destroy(context.surface);
  wl_display_disconnect(context.display);
}

int main() {
  wlContext context;
  if (!connectToWayland(context)) {
    return EXIT_FAILURE;
  }

  try {
    filesystem::path deviceEventFile = findDevice("/dev/input/by-id/");
    if (deviceEventFile.empty()) {
      cout << "Game controller is not connected" << endl;
      return EXIT_FAILURE;
    }

    Gamepad gamepad(deviceEventFile);

    bool isActive = false;
    bool firstIter = true;

    auto lastActiveTime = chrono::steady_clock::now();

    while (true) {
      while (wl_display_prepare_read(context.display) != 0) {
        wl_display_dispatch_pending(context.display);
      }
      wl_display_flush(context.display);

      if (wl_display_read_events(context.display) == -1) {
        cerr << "Failed to read Wayland events" << endl;
        break;
      }

      wl_display_dispatch_pending(context.display);

      auto currentTime = chrono::steady_clock::now();
      gamepad.updateState();

      bool anyButtonPress = gamepad.isAnyButtonPressed();
      bool axisMove = gamepad.isAxisMoved();
      bool triggerPress = gamepad.isAnyTriggerPressed();

      if (anyButtonPress || axisMove || triggerPress) {
        if (!isActive) {
          cout << "controller is active" << endl;
          isActive = true;
          firstIter = false;

          if (!context.idle_inhibitor) {
            context.idle_inhibitor =
                zwp_idle_inhibit_manager_v1_create_inhibitor(
                    context.idle_inhibit_manager, context.surface);
            wl_surface_commit(context.surface);
            cout << "Idle inhibitor created successfully" << endl;
          }
        }
        lastActiveTime = currentTime;
      }

      auto inactiveDuration =
          chrono::duration_cast<chrono::seconds>(currentTime - lastActiveTime);

      if (inactiveDuration.count() >= THRESHOLD) {
        if (isActive) {
          cout << "controller is inactive" << endl;
          isActive = false;
          if (context.idle_inhibitor) {
            zwp_idle_inhibitor_v1_destroy(context.idle_inhibitor);
            wl_surface_commit(context.surface);
            context.idle_inhibitor = 0;
            cout << "Idle inhibitor destroyed successfully" << endl;
          }
        } else if (firstIter) {
          cout << "controller is inactive" << endl;
          firstIter = false;
        }
      }

      this_thread::sleep_for(chrono::milliseconds(10));
    }
  } catch (const exception &e) {
    cerr << "Error: " << e.what() << endl;
    return EXIT_FAILURE;
  }

  clean(context);

  return 0;
}
