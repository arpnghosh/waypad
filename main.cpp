#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <linux/input-event-codes.h>

namespace fs = std::filesystem;
using namespace std;

fs::path findDevice(fs::path inputDeviceFolder) {
  for (const auto &entry : fs::directory_iterator(inputDeviceFolder)) {
    if (fs::is_symlink(entry.path())) {
      fs::path canonicalPath = fs::canonical(entry.path());
      if (canonicalPath.string().find("event-joystick")) {
        return canonicalPath;
      }
    } else {
      cout << "not symlink" << endl;
    }
  }
  return "";
}

int main() {
  struct libevdev *dev = NULL;
  int fd;
  int rc = 1;

  // handle when /by-id/ dir does not exist

  fs::path deviceEventFile = findDevice("/dev/input/by-id/");

  // game controller is not connected

  if (deviceEventFile.empty()) {
    cout << "game controller is not connected";
    return EXIT_FAILURE;
  }

  fd = open(deviceEventFile.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    // failed to open file
  }
  rc = libevdev_new_from_fd(fd, &dev);

  if (rc < 0) {
    cout << "Failed to initialise libevdev \n";
    exit(1);
  }

  printf("Input device name:\"%s\"\n", libevdev_get_name(dev));
  printf("Input device ID: bus %#x vendor %#x product %#x\n",
         libevdev_get_id_bustype(dev), libevdev_get_id_vendor(dev),
         libevdev_get_id_product(dev));

  do {
    struct input_event ev;
    rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    if (rc == 0)
      printf("Event: %s %s %d\n", libevdev_event_type_get_name(ev.type),
             libevdev_event_code_get_name(ev.type, ev.code), ev.value);
  } while (rc == 1 || rc == 0 || rc == -EAGAIN);

  return 0;
}
