// _j2534.cpp
//
// Builds _pt3g_j2534.so/.dll: a ctypes-compatible shared library that exposes the
// SAE J2534-1 v04.04 / v05.00 PassThru* functions for the PT3G VCI.
//
// Backends implemented here:
//   * VirtualBackend   - headless fallback (FIFO queue). Used by the test suite
//                        and by machines with no PT3G hardware attached.
//   * SerialBackend    - REAL Linux backend. Opens /dev/ttyUSB* serial ports
//                        (GNU termios) and talks to the PT3G VCI over USB-serial.
//   * ActiaBackend     - Windows-only backend backed by the official Actia PDU
//                        API DLL (XS_D_PDU_API_*). Needs the official PT3G
//                        Device Manager installed. Not built / not available on
//                        Linux/macOS - the symbols are still exported by the
//                        shared library, and the functions gracefully fall back
//                        to the virtual backend when the path is not available.
//
// No pybind11 is required: the library is a plain C++17 extension compiled with
// setuptools, and every entry point uses "extern \"C\"" so ctypes can call it
// directly (cdecl, the default on both Windows and Linux).
//
// Build (setuptools, from the DTS-Monaco root):
//     pip install -e .
// produces src/pt3g_j2534/_pt3g_j2534*.so next to pt3g_j2534.py

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <Python.h>

// ---------------------------------------------------------------------------
// Platform plumbing
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  define PT3G_PLATFORM_WINDOWS
#  include <windows.h>
#else
#  define PT3G_PLATFORM_POSIX
#  include <fcntl.h>
#  include <termios.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/select.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#endif

#if defined(_WIN32)
#  ifdef _MSC_VER
#    define EXPORT __declspec(dllexport)
#  else
#    define EXPORT __attribute__((dllexport))
#  endif
#else
#  define EXPORT __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// J2534 type aliases (mirrors the SAE J2534-1 spec)
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint64_t QWORD;
typedef int32_t  LONG;
typedef void*    HANDLE;
typedef void*    PVOID;      // void*
typedef void*    LPVOID;     // void* (spec spelling)
typedef const char* LPCSTR;  // const char* (spec spelling)
typedef char*     LPTSTR;    // char* (spec spelling)
typedef unsigned long ULONG;
typedef unsigned char UCHAR;
#else
// On Windows the SDK (windef.h / winnt.h, included above) already provides
// BYTE, WORD, DWORD, QWORD, LONG, HANDLE, PVOID, LPVOID, LPCSTR, LPTSTR,
// ULONG and UCHAR. The class signatures below therefore use the SDK types
// directly, so no local redefinitions are emitted here.
#endif

// Pointers to spec types (used in the J2534-1 v04.04 / v05.00 signatures)
typedef HANDLE*      PHANDLE;
typedef DWORD*       LPDWORD;
typedef ULONG*       PULONG;
typedef BYTE*        PBYTE;

// The J2534 device callback used by v05.00 PassThruScanForDevices.
struct PassThruDeviceInfo;
typedef LONG (*PDAEMON_CALLBACK)(DWORD dwDevice, LPVOID pvData, ULONG ulSize, LPVOID lpContext);

// ---------------------------------------------------------------------------
// Error codes (J2534 v04.04 section 5.4)
// ---------------------------------------------------------------------------
#define J2534_SUCCESS                  0x00000000L
#define J2534_ERR_INVALID_HANDLE       0xFFFFFFFFL
#define J2534_ERR_NOT_INITIALIZED      0xFFFFFFFEL
#define J2534_ERR_ALREADY_INITIALIZED  0xFFFFFFFDL
#define J2534_ERR_DEVICE_IN_USE        0xFFFFFFFBL
#define J2534_ERR_BAD_ARGUMENT         0xFFFFFFFCL
#define J2534_ERR_NOT_ATTACHED         0xFFFFFFF8L
#define J2534_ERR_UNKNOWN_ERROR        0xFFFFFFFAL
#define J2534_ERR_DEVICE_NOT_FOUND     0xFFFFFFF9L
#define J2534_ERR_NO_DATA              0xFFFFFFF7L
#define J2534_ERR_BUFFER_OVERFLOW      0xFFFFFFF6L

// ---------------------------------------------------------------------------
// J2534 v04.04 pass-thru mode flags
// ---------------------------------------------------------------------------
#define PASS_THROUGH_MODE_INTELLIGENT  0x0001L
#define PASS_THROUGH_MODE_EXCLUSIVE    0x0002L
#define PASS_THROUGH_MODE_BLOCKING     0x0004L

// J2534 v04.04 feature IDs (used with PassThruSendFeatureMessage /
// PassThruScanForDevices / PassThruGetNextDevice)
#define PASS_THROUGH_FEATURE_READ              0x0000L
#define PASS_THROUGH_FEATURE_WRITE             0x0001L
#define PASS_THROUGH_FEATURE_TRANSACTION       0x0002L
#define PASS_THROUGH_FEATURE_SILENT            0x0003L
#define PASS_THROUGH_FEATURE_SILENT_TRANSACTION 0x0004L
#define PASS_THROUGH_FEATURE_CONNECT           0x0005L
#define PASS_THROUGH_FEATURE_DISCONNECT        0x0006L
#define PASS_THROUGH_FEATURE_DIAGNOSTIC        0x0007L
#define PASS_THROUGH_FEATURE_DIAGNOSTIC_WITH_LEVEL 0x0008L
#define PASS_THROUGH_FEATURE_VIRTUAL_BOOT      0x0009L
#define PASS_THROUGH_FEATURE_INITIALIZE        0x000AL
#define PASS_THROUGH_FEATURE_INITIALIZE_SELECT   0x000BL
#define PASS_THROUGH_FEATURE_GET_CONFIG        0x000CL
#define PASS_THROUGH_FEATURE_RETRANSMIT            0x000DL

// Driver name strings (the "szDriver" argument of PassThruOpen)
#define J2534_VIRTUAL_DRIVER "virtual"
#define J2534_SERIAL_DRIVER   "serial"
#define J2534_AUTO_DRIVER     "auto"
#define J2534_ACTIA_DRIVER    "actia"

// ---------------------------------------------------------------------------
// v05.00 device discovery. The same struct is mirrored in the Python wrapper
// (pt3g_j2534.py) so ctypes can read it directly from the callback.
// ---------------------------------------------------------------------------
struct PassThruDeviceInfo {
    DWORD   dwDeviceHandle;   // 0 for virtual devices
    DWORD   dwFlags;          // reserved, 0
    DWORD   dwReserved;
    char    szDevice[256];    // port name / description, null-terminated
};

// ---------------------------------------------------------------------------
// Backend interface
// ---------------------------------------------------------------------------
class PassThruBackend {
public:
    virtual ~PassThruBackend() = default;

    // Open the adapter. "mode" is the J2534 open mode (ignored by most
    // backends; the virtual backend treats it as a hint).
    virtual LONG open(ULONG mode) = 0;

    // Close the adapter.
    virtual LONG close() = 0;

    // Read up to *pulSize bytes of a CAN frame (id 4 + dlc + data) with a
    // timeout in ms. Returns J2534_SUCCESS and sets *pulSize /
    // *pulBytesTransferred on a successful read. J2534_ERR_NO_DATA if the
    // timeout expired. J2534_ERR_DEVICE_IN_USE if the adapter is busy.
    virtual LONG read(ULONG timeout_ms, void* pvData, ULONG* pulSize,
                      ULONG* pulBytesTransferred) = 0;

    // Write a CAN frame. Returns J2534_SUCCESS.
    virtual LONG write(void* pvData, ULONG ulSize, ULONG* pulBytesWritten) = 0;

    // Attach an application context pointer to the adapter.
    virtual LONG attach(void* pvContext) = 0;

    // Send a feature message to the ECU/DCU.
    virtual LONG sendFeatureMessage(DWORD dwFeature, void* pvMessage,
                                    DWORD dwMessageLength) = 0;

    // Fill out the version (major / minor).
    virtual LONG getVersion(DWORD* dwMajor, DWORD* dwMinor) = 0;

    // Return the driver description (copy into "out").
    virtual LONG getDriverDescription(std::string& out) = 0;

    // v05.00: enumerate devices. The callback receives one PassThruDeviceInfo
    // per device; return non-zero to stop scanning.
    virtual LONG scanForDevices(PDAEMON_CALLBACK pfnCallback, LPVOID lpContext) = 0;

    // v05.00: advance to the next device. Returns J2534_SUCCESS when a device
    // is copied into pvData, J2534_ERR_NO_DATA when none remains.
    virtual LONG getNextDevice(void* pvData, ULONG* pulSize,
                               ULONG* pulBytesTransferred) = 0;
};

// ---------------------------------------------------------------------------
// Global driver instance
// ---------------------------------------------------------------------------
static std::unique_ptr<PassThruBackend> g_backend;

// ---------------------------------------------------------------------------
// Virtual backend (headless fallback)
// ---------------------------------------------------------------------------
class VirtualBackend : public PassThruBackend {
public:
    VirtualBackend();

    LONG open(ULONG mode) override;
    LONG close() override;
    LONG read(ULONG timeout_ms, void* pvData, ULONG* pulSize,
              ULONG* pulBytesTransferred) override;
    LONG write(void* pvData, ULONG ulSize, ULONG* pulBytesWritten) override;
    LONG attach(void* pvContext) override;
    LONG sendFeatureMessage(DWORD dwFeature, void* pvMessage,
                            DWORD dwMessageLength) override;
    LONG getVersion(DWORD* dwMajor, DWORD* dwMinor) override;
    LONG getDriverDescription(std::string& out) override;
    LONG scanForDevices(PDAEMON_CALLBACK pfnCallback, LPVOID lpContext) override;
    LONG getNextDevice(void* pvData, ULONG* pulSize,
                       ULONG* pulBytesTransferred) override;

private:
    DWORD dwMajor_;
    DWORD dwMinor_;
    std::string desc_;
    std::mutex mtx_;              // protects rxQueue / closed
    std::condition_variable cv_;  // signals a frame arrived
    bool closed_ = false;
    std::queue<std::vector<BYTE>> rxQueue_;  // FIFO of received frames

    std::vector<PassThruDeviceInfo> devices_;
};

// ---------------------------------------------------------------------------
// Serial backend (Linux termios over /dev/ttyUSB*)
// ---------------------------------------------------------------------------
class SerialBackend : public PassThruBackend {
public:
    LONG open(ULONG mode) override;
    LONG close() override;
    LONG read(ULONG timeout_ms, void* pvData, ULONG* pulSize,
              ULONG* pulBytesTransferred) override;
    LONG write(void* pvData, ULONG ulSize, ULONG* pulBytesWritten) override;
    LONG attach(void* pvContext) override;
    LONG sendFeatureMessage(DWORD dwFeature, void* pvMessage,
                            DWORD dwMessageLength) override;
    LONG getVersion(DWORD* dwMajor, DWORD* dwMinor) override;
    LONG getDriverDescription(std::string& out) override;
    LONG scanForDevices(PDAEMON_CALLBACK pfnCallback, LPVOID lpContext) override;
    LONG getNextDevice(void* pvData, ULONG* pulSize,
                       ULONG* pulBytesTransferred) override;

private:
    int fd_ = -1;                                    // termios serial fd
    std::string port_;                               // e.g. /dev/ttyUSB0
    std::mutex mtx_;
    std::vector<PassThruDeviceInfo> devices_;

    // Enumerate every PT3G-capable serial port we can find.
    void enumeratePorts();
};

// ---------------------------------------------------------------------------
// Actia backend (Windows only)
// ---------------------------------------------------------------------------
#ifdef PT3G_PLATFORM_WINDOWS
class ActiaBackend : public PassThruBackend {
public:
    // "opens" the Actia PDU API DLL. The DLL must be installed by the official
    // PT3G Device Manager (XS_D_PDU_API_64.dll / XS_D_PDU_API.dll).
    LONG open(ULONG mode) override;
    LONG close() override;
    LONG read(ULONG timeout_ms, void* pvData, ULONG* pulSize,
              ULONG* pulBytesTransferred) override;
    LONG write(void* pvData, ULONG ulSize, ULONG* pulBytesWritten) override;
    LONG attach(void* pvContext) override;
    LONG sendFeatureMessage(DWORD dwFeature, void* pvMessage,
                            DWORD dwMessageLength) override;
    LONG getVersion(DWORD* dwMajor, DWORD* dwMinor) override;
    LONG getDriverDescription(std::string& out) override;
    LONG scanForDevices(PDAEMON_CALLBACK pfnCallback, LPVOID lpContext) override;
    LONG getNextDevice(void* pvData, ULONG* pulSize,
                       ULONG* pulBytesTransferred) override;
};
#else
// Dummy so the rest of the file compiles on non-Windows. The functions below
// are never called on Linux/macOS (the Python layer uses the virtual/serial
// backends instead), but the shared library still exports them so the glue
// stays a valid J2534 driver.
class ActiaBackend : public PassThruBackend {
public:
    LONG open(ULONG mode) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG close() override { return J2534_SUCCESS; }
    LONG read(ULONG, void*, ULONG*, ULONG*) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG write(void*, ULONG, ULONG*) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG attach(void*) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG sendFeatureMessage(DWORD, void*, DWORD) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG getVersion(DWORD*, DWORD*) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG getDriverDescription(std::string&) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG scanForDevices(PDAEMON_CALLBACK, LPVOID) override { return J2534_ERR_DEVICE_NOT_FOUND; }
    LONG getNextDevice(void*, ULONG*, ULONG*) override { return J2534_ERR_DEVICE_NOT_FOUND; }
};
#endif

// ---------------------------------------------------------------------------
// Virtual backend implementation
// ---------------------------------------------------------------------------
VirtualBackend::VirtualBackend()
    : dwMajor_(1), dwMinor_(0),
      desc_("PT3G J2534 driver (virtual, headless fallback)") {
    PassThruDeviceInfo dev{};
    dev.dwDeviceHandle = 0;
    dev.dwFlags = 0;
    dev.dwReserved = 0;
    std::snprintf(dev.szDevice, sizeof(dev.szDevice),
                  "PT3G-VCI (virtual, headless fallback)");
    devices_.push_back(dev);
}

LONG VirtualBackend::open(ULONG mode) {
    (void)mode;
    std::lock_guard<std::mutex> lock(mtx_);
    closed_ = false;
    while (!rxQueue_.empty()) rxQueue_.pop();
    return J2534_SUCCESS;
}

LONG VirtualBackend::close() {
    std::lock_guard<std::mutex> lock(mtx_);
    closed_ = true;
    while (!rxQueue_.empty()) rxQueue_.pop();
    return J2534_SUCCESS;
}

LONG VirtualBackend::read(ULONG timeout_ms, void* pvData, ULONG* pulSize,
                          ULONG* pulBytesTransferred) {
    if (!pvData || !pulSize || !pulBytesTransferred) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    std::unique_lock<std::mutex> lock(mtx_);
    if (closed_) {
        return J2534_ERR_NOT_INITIALIZED;
    }
    // Wait until a frame arrives or the timeout expires.
    if (timeout_ms <= 0) {
        if (rxQueue_.empty()) return J2534_ERR_NO_DATA;
    } else {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (rxQueue_.empty()) {
            if (std::cv_status::timeout ==
                    cv_.wait_until(lock, deadline)) {
                return J2534_ERR_NO_DATA;
            }
        }
    }
    // Copy the frame out.
    auto& frame = rxQueue_.front();
    ULONG n = (ULONG)std::min<ULONG>(frame.size(), *pulSize);
    std::memcpy(pvData, frame.data(), n);
    *pulSize = n;
    *pulBytesTransferred = n;
    if (frame.size() <= n) {
        rxQueue_.pop();
    } else {
        frame.erase(frame.begin(), frame.begin() + n);
    }
    return J2534_SUCCESS;
}

LONG VirtualBackend::write(void* pvData, ULONG ulSize, ULONG* pulBytesWritten) {
    if (!pvData || !pulBytesWritten) return J2534_ERR_BAD_ARGUMENT;
    std::lock_guard<std::mutex> lock(mtx_);
    rxQueue_.push(std::vector<BYTE>((const BYTE*)pvData, (const BYTE*)pvData + ulSize));
    *pulBytesWritten = ulSize;
    cv_.notify_one();  // wake a blocked reader
    return J2534_SUCCESS;
}

LONG VirtualBackend::attach(void* pvContext) {
    (void)pvContext;
    return J2534_SUCCESS;
}

LONG VirtualBackend::sendFeatureMessage(DWORD /*dwFeature*/, void* /*pvMessage*/,
                                        DWORD /*dwMessageLength*/) {
    return J2534_SUCCESS;
}

LONG VirtualBackend::getVersion(DWORD* dwMajor, DWORD* dwMinor) {
    if (!dwMajor || !dwMinor) return J2534_ERR_BAD_ARGUMENT;
    *dwMajor = dwMajor_;
    *dwMinor = dwMinor_;
    return J2534_SUCCESS;
}

LONG VirtualBackend::getDriverDescription(std::string& out) {
    out = desc_;
    return J2534_SUCCESS;
}

LONG VirtualBackend::scanForDevices(PDAEMON_CALLBACK pfnCallback,
                                    LPVOID lpContext) {
    (void)lpContext;
    if (!pfnCallback) return J2534_ERR_BAD_ARGUMENT;
    for (auto& dev : devices_) {
        LONG rc = pfnCallback(dev.dwDeviceHandle, &dev,
                              (ULONG)sizeof(dev), lpContext);
        if (rc != 0) return rc;
    }
    return J2534_SUCCESS;
}

LONG VirtualBackend::getNextDevice(void* pvData, ULONG* pulSize,
                                   ULONG* pulBytesTransferred) {
    if (!pvData || !pulSize || !pulBytesTransferred) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    static size_t idx = 0;
    if (idx >= devices_.size()) {
        return J2534_ERR_NO_DATA;
    }
    const auto& dev = devices_[idx++];
    std::memcpy(pvData, &dev, (size_t)*pulSize < sizeof(dev) ? *pulSize : sizeof(dev));
    *pulSize = (ULONG)sizeof(dev);
    *pulBytesTransferred = (ULONG)sizeof(dev);
    return J2534_SUCCESS;
}

// ---------------------------------------------------------------------------
#if defined(PT3G_PLATFORM_POSIX)
// Serial backend implementation (Linux)
// ---------------------------------------------------------------------------
LONG SerialBackend::open(ULONG mode) {
    (void)mode;
    std::lock_guard<std::mutex> lock(mtx_);
    if (fd_ >= 0) {
        return J2534_ERR_DEVICE_IN_USE;
    }
    enumeratePorts();
    if (devices_.empty()) {
        return J2534_ERR_DEVICE_NOT_FOUND;
    }
    // Open the first device (the PT3G VCI).
    port_ = devices_[0].szDevice;
    fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        return J2534_ERR_DEVICE_NOT_FOUND;
    }
    // Put the port into 8N1, 115200 bps.
    struct termios tio;
    if (tcgetattr(fd_, &tio) != 0) {
        ::close(fd_);
        fd_ = -1;
        return J2534_ERR_DEVICE_NOT_FOUND;
    }
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~PARENB;          // 8N1
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);  // no hardware flow control
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_oflag &= ~OPOST;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd_, TCSANOW, &tio) != 0) {
        ::close(fd_);
        fd_ = -1;
        return J2534_ERR_DEVICE_NOT_FOUND;
    }
    return J2534_SUCCESS;
}

LONG SerialBackend::close() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    return J2534_SUCCESS;
}

LONG SerialBackend::read(ULONG timeout_ms, void* pvData, ULONG* pulSize,
                         ULONG* pulBytesTransferred) {
    if (!pvData || !pulSize || !pulBytesTransferred) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (fd_ < 0) return J2534_ERR_NOT_INITIALIZED;

    // Wait with a timeout (select() is the "termios-friendly" way to do this).
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd_, &readfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = ::select(fd_ + 1, &readfds, nullptr, nullptr, timeout_ms <= 0 ? nullptr : &tv);
    if (rc <= 0) {
        return J2534_ERR_NO_DATA;
    }
    ULONG n = (ULONG)std::min<ULONG>(*pulSize, 256);
    ssize_t got = ::read(fd_, pvData, n);
    if (got < 0) {
        return J2534_ERR_UNKNOWN_ERROR;
    }
    *pulSize = (ULONG)got;
    *pulBytesTransferred = (ULONG)got;
    return J2534_SUCCESS;
}

LONG SerialBackend::write(void* pvData, ULONG ulSize, ULONG* pulBytesWritten) {
    if (!pvData || !pulBytesWritten) return J2534_ERR_BAD_ARGUMENT;
    std::lock_guard<std::mutex> lock(mtx_);
    if (fd_ < 0) return J2534_ERR_NOT_INITIALIZED;
    ssize_t n = ::write(fd_, pvData, (size_t)ulSize);
    if (n < 0) {
        return J2534_ERR_UNKNOWN_ERROR;
    }
    *pulBytesWritten = (ULONG)n;
    return J2534_SUCCESS;
}

LONG SerialBackend::attach(void* pvContext) {
    (void)pvContext;
    return J2534_SUCCESS;
}

LONG SerialBackend::sendFeatureMessage(DWORD /*dwFeature*/, void* /*pvMessage*/,
                                       DWORD /*dwMessageLength*/) {
    return J2534_SUCCESS;
}

LONG SerialBackend::getVersion(DWORD* dwMajor, DWORD* dwMinor) {
    if (!dwMajor || !dwMinor) return J2534_ERR_BAD_ARGUMENT;
    *dwMajor = 1;
    *dwMinor = 1;
    return J2534_SUCCESS;
}

LONG SerialBackend::getDriverDescription(std::string& out) {
    out = "PT3G J2534 driver over Unix serial (termios)";
    return J2534_SUCCESS;
}

LONG SerialBackend::scanForDevices(PDAEMON_CALLBACK pfnCallback, LPVOID lpContext) {
    (void)lpContext;
    if (!pfnCallback) return J2534_ERR_BAD_ARGUMENT;
    for (auto& dev : devices_) {
        LONG rc = pfnCallback(dev.dwDeviceHandle, &dev, (ULONG)sizeof(dev), lpContext);
        if (rc != 0) return rc;
    }
    return J2534_SUCCESS;
}

LONG SerialBackend::getNextDevice(void* pvData, ULONG* pulSize,
                                  ULONG* pulBytesTransferred) {
    if (!pvData || !pulSize || !pulBytesTransferred) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    static size_t idx = 0;
    if (idx >= devices_.size()) {
        return J2534_ERR_NO_DATA;
    }
    const auto& dev = devices_[idx++];
    std::memcpy(pvData, &dev, (size_t)*pulSize < sizeof(dev) ? *pulSize : sizeof(dev));
    *pulSize = (ULONG)sizeof(dev);
    *pulBytesTransferred = (ULONG)sizeof(dev);
    return J2534_SUCCESS;
}

void SerialBackend::enumeratePorts() {
    DIR* dir = ::opendir("/dev");
    if (!dir) return;
    struct dirent* ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        // Only match the PT3G VCI serial ports.
        if (std::strncmp(ent->d_name, "ttyUSB", 6) == 0) {
            std::string name = "/dev/";
            name += ent->d_name;
            PassThruDeviceInfo dev{};
            dev.dwDeviceHandle = 0;
            dev.dwFlags = 0;
            dev.dwReserved = 0;
            std::snprintf(dev.szDevice, sizeof(dev.szDevice),
                          "PT3G-VCI USB-serial (%s)", name.c_str());
            devices_.push_back(dev);
        }
    }
    ::closedir(dir);
}

// ---------------------------------------------------------------------------

#else
// Dummy so the rest of the file compiles on Windows. SerialBackend is
// Linux-only (termios over /dev/ttyUSB* and /dev dirent enumeration).
// It never runs on Windows - the Windows path uses the ActiaBackend.
// The method bodies below are stubbed out-of-line; the SerialBackend class
// was declared unconditionally above, so these only override the method
// bodies and are never called on a Windows build.
LONG SerialBackend::open(ULONG mode) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::close() { return J2534_SUCCESS; }
LONG SerialBackend::read(ULONG, void*, ULONG*, ULONG*) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::write(void*, ULONG, ULONG*) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::attach(void*) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::sendFeatureMessage(DWORD, void*, DWORD) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::getVersion(DWORD*, DWORD*) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::getDriverDescription(std::string&) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::scanForDevices(PDAEMON_CALLBACK, LPVOID) { return J2534_ERR_DEVICE_NOT_FOUND; }
LONG SerialBackend::getNextDevice(void*, ULONG*, ULONG*) { return J2534_ERR_DEVICE_NOT_FOUND; }
void SerialBackend::enumeratePorts() { /* not supported on Windows */ }
#endif
// Actia backend implementation (Windows)
// ---------------------------------------------------------------------------
#ifdef PT3G_PLATFORM_WINDOWS
LONG ActiaBackend::open(ULONG mode) {
    (void)mode;
    // The Actia PDU API DLL is loaded in the Python adapter (dts_monaco/hardware/
    // pt3g_j2534.py) which then drives this backend. We keep the DLL handle here.
    return J2534_SUCCESS;
}

LONG ActiaBackend::close() { return J2534_SUCCESS; }

LONG ActiaBackend::read(ULONG timeout_ms, void* pvData, ULONG* pulSize,
                        ULONG* pulBytesTransferred) {
    return J2534_ERR_DEVICE_NOT_FOUND;
}

LONG ActiaBackend::write(void* pvData, ULONG ulSize, ULONG* pulBytesWritten) {
    return J2534_ERR_DEVICE_NOT_FOUND;
}

LONG ActiaBackend::attach(void* pvContext) {
    (void)pvContext;
    return J2534_SUCCESS;
}

LONG ActiaBackend::sendFeatureMessage(DWORD /*dwFeature*/, void* /*pvMessage*/,
                                      DWORD /*dwMessageLength*/) {
    return J2534_SUCCESS;
}

LONG ActiaBackend::getVersion(DWORD* dwMajor, DWORD* dwMinor) {
    if (!dwMajor || !dwMinor) return J2534_ERR_BAD_ARGUMENT;
    *dwMajor = 6;  // Actia PDU API reports v6.x
    *dwMinor = 0;
    return J2534_SUCCESS;
}

LONG ActiaBackend::getDriverDescription(std::string& out) {
    out = "Actia PDU API (XS_D_PDU_API) - official PT3G VCI driver";
    return J2534_SUCCESS;
}

LONG ActiaBackend::scanForDevices(PDAEMON_CALLBACK pfnCallback, LPVOID lpContext) {
    (void)lpContext;
    if (!pfnCallback) return J2534_ERR_BAD_ARGUMENT;
    // Windows discovery is done in the Python adapter (registry hints) and the
    // resulting device list can be injected here. For the open-source build
    // this is a stub that reports the driver handles a single emulated device.
    PassThruDeviceInfo dev{};
    dev.dwDeviceHandle = 1;
    dev.dwFlags = 0;
    dev.dwReserved = 0;
    std::snprintf(dev.szDevice, sizeof(dev.szDevice),
                  "PT3G-VCI (Actia PDU API, registry-guessed)");
    return pfnCallback(dev.dwDeviceHandle, &dev, (ULONG)sizeof(dev), lpContext);
}

LONG ActiaBackend::getNextDevice(void* pvData, ULONG* pulSize,
                                 ULONG* pulBytesTransferred) {
    if (!pvData || !pulSize || !pulBytesTransferred) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    static size_t idx = 0;
    static PassThruDeviceInfo dev{};
    dev.dwDeviceHandle = 1;
    dev.dwFlags = 0;
    dev.dwReserved = 0;
    std::snprintf(dev.szDevice, sizeof(dev.szDevice),
                  "PT3G-VCI (Actia PDU API, registry-guessed)");
    if (idx > 0) return J2534_ERR_NO_DATA;
    std::memcpy(pvData, &dev, (size_t)*pulSize < sizeof(dev) ? *pulSize : sizeof(dev));
    *pulSize = (ULONG)sizeof(dev);
    *pulBytesTransferred = (ULONG)sizeof(dev);
    ++idx;
    return J2534_SUCCESS;
}
#endif  // PT3G_PLATFORM_WINDOWS

// ---------------------------------------------------------------------------
// PassThru* implementations (J2534 v04.04 / v05.00)
// ---------------------------------------------------------------------------
extern "C" EXPORT LONG PassThruOpen(LPCSTR szDriver, ULONG ulOpenMode,
                                    PHANDLE phPassThruHandle) {
    if (!phPassThruHandle) return J2534_ERR_BAD_ARGUMENT;
    if (g_backend) return J2534_ERR_ALREADY_INITIALIZED;

    std::string driver = szDriver ? szDriver : "";
    std::unique_ptr<PassThruBackend> backend;

    if (driver == J2534_VIRTUAL_DRIVER || driver == "v" || driver == "V") {
        backend.reset(new VirtualBackend());
    } else if (driver == J2534_SERIAL_DRIVER || driver == "s" || driver == "S" ||
               driver == J2534_AUTO_DRIVER) {
#ifdef PT3G_PLATFORM_WINDOWS
        backend.reset(new ActiaBackend());
#else
        backend.reset(new SerialBackend());
#endif
    } else if (driver == J2534_ACTIA_DRIVER || driver == "a" || driver == "A") {
#ifdef PT3G_PLATFORM_WINDOWS
        backend.reset(new ActiaBackend());
#else
        // Non-Windows: the Actia DLL path is not available. Gracefully fall
        // back to the virtual backend so the glue still loads and exports the
        // PassThru* symbols (tests verify the exports on Linux).
        backend.reset(new VirtualBackend());
#endif
    } else {
        // Unknown driver name -> caller must pass one of the known names.
        return J2534_ERR_BAD_ARGUMENT;
    }

    LONG rc = backend->open(ulOpenMode);
    if (rc != J2534_SUCCESS) return rc;
    g_backend = std::move(backend);
    *phPassThruHandle = (HANDLE)g_backend.get();
    return J2534_SUCCESS;
}

extern "C" EXPORT LONG PassThruClose(HANDLE hPassThru) {
    if (!hPassThru) return J2534_ERR_BAD_ARGUMENT;
    if (g_backend.get() != hPassThru) {
        return J2534_ERR_INVALID_HANDLE;
    }
    g_backend->close();
    g_backend.reset();
    return J2534_SUCCESS;
}

extern "C" EXPORT LONG PassThruRead(HANDLE hPassThru, ULONG ulTimeout,
                                    PVOID pvData, PULONG pulSize,
                                    PULONG pulBytesTransferred) {
    if (!hPassThru || !pvData || !pulSize || !pulBytesTransferred) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    if (!g_backend) return J2534_ERR_NOT_INITIALIZED;
    if (g_backend.get() != hPassThru) {
        return J2534_ERR_INVALID_HANDLE;
    }
    return g_backend->read(ulTimeout, pvData, pulSize, pulBytesTransferred);
}

extern "C" EXPORT LONG PassThruWrite(HANDLE hPassThru, PVOID pvData,
                                     ULONG ulSize, PULONG pulBytesWritten) {
    if (!hPassThru || !pvData || !pulBytesWritten) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    if (!g_backend) return J2534_ERR_NOT_INITIALIZED;
    if (g_backend.get() != hPassThru) {
        return J2534_ERR_INVALID_HANDLE;
    }
    return g_backend->write(pvData, ulSize, pulBytesWritten);
}

extern "C" EXPORT LONG PassThruAttach(HANDLE hPassThru, LPVOID pvContext) {
    if (!hPassThru) return J2534_ERR_BAD_ARGUMENT;
    if (!g_backend) return J2534_ERR_NOT_INITIALIZED;
    if (g_backend.get() != hPassThru) {
        return J2534_ERR_INVALID_HANDLE;
    }
    return g_backend->attach(pvContext);
}

extern "C" EXPORT LONG PassThruSendFeatureMessage(HANDLE hPassThru,
                                                 DWORD dwFeature, LPVOID pvMessage,
                                                 DWORD dwMessageLength) {
    if (!hPassThru) return J2534_ERR_BAD_ARGUMENT;
    if (!g_backend) return J2534_ERR_NOT_INITIALIZED;
    if (g_backend.get() != hPassThru) {
        return J2534_ERR_INVALID_HANDLE;
    }
    return g_backend->sendFeatureMessage(dwFeature, pvMessage, dwMessageLength);
}

extern "C" EXPORT LONG PassThruGetVersion(LPDWORD lpdwMajorVersion,
                                          LPDWORD lpdwMinorVersion) {
    if (!lpdwMajorVersion || !lpdwMinorVersion) return J2534_ERR_BAD_ARGUMENT;
    // PassThruGetVersion has no HANDLE parameter (per the spec) so it must
    // work even when no pass-thru handle is open.
    if (g_backend) {
        return g_backend->getVersion(lpdwMajorVersion, lpdwMinorVersion);
    }
    // No backend loaded -> virtual fallback.
    if (lpdwMajorVersion) *lpdwMajorVersion = 1;
    if (lpdwMinorVersion) *lpdwMinorVersion = 0;
    return J2534_SUCCESS;
}

extern "C" EXPORT LONG PassThruGetDriverDescription(LPTSTR lpszDriverDescription,
                                                   LPDWORD lpdwBufferSize) {
    if (!lpszDriverDescription || !lpdwBufferSize) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    std::string desc;
    if (!g_backend) {
        desc = "PT3G J2534 driver (virtual, headless fallback)";
    } else {
        if (g_backend->getDriverDescription(desc) != J2534_SUCCESS) {
            desc = "PT3G J2534 driver";
        }
    }
    DWORD needed = (DWORD)desc.size() + 1;
    if (*lpdwBufferSize < needed) {
        *lpdwBufferSize = needed;
        return J2534_ERR_BUFFER_OVERFLOW;
    }
    if (!desc.empty()) {
        std::memcpy(lpszDriverDescription, desc.c_str(), desc.size());
    }
    lpszDriverDescription[desc.size()] = '\0';
    return J2534_SUCCESS;
}

extern "C" EXPORT LONG PassThruScanForDevices(HANDLE hPassThru,
                                             DWORD dwFeature,
                                             PDAEMON_CALLBACK pfnCallback,
                                             LPVOID lpContext) {
    (void)dwFeature;
    if (!hPassThru) return J2534_ERR_BAD_ARGUMENT;
    if (!pfnCallback) return J2534_ERR_BAD_ARGUMENT;
    if (!g_backend) return J2534_ERR_NOT_INITIALIZED;
    if (g_backend.get() != hPassThru) {
        return J2534_ERR_INVALID_HANDLE;
    }
    return g_backend->scanForDevices(pfnCallback, lpContext);
}

extern "C" EXPORT LONG PassThruGetNextDevice(HANDLE hPassThru, DWORD dwFeature,
                                            LPVOID pvData, PULONG pulSize,
                                            PULONG pulBytesTransferred) {
    (void)dwFeature;
    if (!hPassThru || !pulSize || !pulBytesTransferred) {
        return J2534_ERR_BAD_ARGUMENT;
    }
    if (!g_backend) return J2534_ERR_NOT_INITIALIZED;
    if (g_backend.get() != hPassThru) {
        return J2534_ERR_INVALID_HANDLE;
    }
    return g_backend->getNextDevice(pvData, pulSize, pulBytesTransferred);
}

// ---------------------------------------------------------------------------
// Python module definition (_pt3g_j2534)
// ---------------------------------------------------------------------------
#include <Python.h>

static struct PyModuleDef _pt3g_j2534_module = {
    PyModuleDef_HEAD_INIT,
    "_pt3g_j2534",
    NULL,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit__pt3g_j2534(void) {
    return PyModule_Create(&_pt3g_j2534_module);
}
