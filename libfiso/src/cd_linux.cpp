// Linux adapter: CD-DA reading with MMC commands sent through the SG_IO interface of the
// SCSI generic layer (works with /dev/srN for SATA, USB and SCSI drives).
// Commands: READ TOC/PMA/ATIP (43h), READ CD (BEh), READ SUB-CHANNEL (42h).
// SPDX-License-Identifier: Apache-2.0
#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

#include "cd.h"
#include "cd_mmc.h"

namespace fiso {

namespace {

std::string readSysfs(const std::string &path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// Block devices of the optical drives: /dev/sr0, /dev/sr1…
std::vector<std::string> opticalDrives() {
    std::vector<std::string> v;
    if (DIR *d = opendir("/sys/class/block")) {
        while (auto *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("sr", 0) == 0) v.push_back(n);
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}

int openDrive(const std::string &dev) {
    int fd = open(dev.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            fail(FISO_E_IO,
                 "[permission] cannot open " + dev + ": add your user to the group that owns it (usually 'cdrom' or 'optical')");
        if (errno == EBUSY) fail(FISO_E_IO, "[busy] " + dev + " is used by another application");
        fail(FISO_E_IO, "[no_device] cannot open " + dev + ": " + strerror(errno));
    }
    return fd;
}

// MMC transport over SG_IO.
class SgTransport : public cd::MmcTransport {
  public:
    explicit SgTransport(int fd) : fd_(fd) {}
    long command(const uint8_t *cdb, size_t cdbLen, uint8_t *data, size_t dataLen) override {
        sg_io_hdr_t h{};
        uint8_t sense[32] = {};
        h.interface_id = 'S';
        h.cmdp = const_cast<uint8_t *>(cdb);
        h.cmd_len = (unsigned char)cdbLen;
        h.dxfer_direction = dataLen ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
        h.dxferp = data;
        h.dxfer_len = (unsigned)dataLen;
        h.sbp = sense;
        h.mx_sb_len = sizeof sense;
        h.timeout = 30000;  // ms; slow drives spin up and retry internally
        if (ioctl(fd_, SG_IO, &h) != 0 || (h.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
        return (long)dataLen - (h.resid > 0 ? h.resid : 0);
    }

  private:
    int fd_;
};

class LinuxReader : public cd::MmcReader {
  public:
    // The base class only keeps a reference to transport_, initialised right after it.
    LinuxReader(const std::string &name, int fd) : cd::MmcReader(name, transport_), fd_(fd), transport_(fd) {}
    ~LinuxReader() override { close(fd_); }

  private:
    int fd_;
    SgTransport transport_;
};

std::string driveName(const std::string &sr) {
    std::string vendor = readSysfs("/sys/class/block/" + sr + "/device/vendor");
    std::string model = readSysfs("/sys/class/block/" + sr + "/device/model");
    std::string name = vendor.empty() ? model : vendor + " " + model;
    return name.empty() ? "CD drive (" + sr + ")" : name;
}

std::string mediaState(int fd) {
    int st = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    switch (st) {
        case CDS_DISC_OK:
            return "present";
        case CDS_DRIVE_NOT_READY:
            return "transition";
        default:
            return "none";
    }
}

std::string pickDevice(const std::string &id) {
    auto drives = opticalDrives();
    if (!id.empty()) {
        for (auto &sr : drives)
            if ("/dev/" + sr == id || sr == id) return "/dev/" + sr;
        fail(FISO_E_ARG, "[no_device] drive '" + id + "' not found");
    }
    if (drives.empty()) fail(FISO_E_UNSUPPORTED, "[no_device] no CD drive was found");
    for (auto &sr : drives) {  // the first drive that holds a disc
        int fd = open(("/dev/" + sr).c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        bool present = mediaState(fd) == "present";
        close(fd);
        if (present) return "/dev/" + sr;
    }
    return "/dev/" + drives[0];
}

}  // namespace

json platformDrives() {
    json out = json::array();
    for (auto &sr : opticalDrives()) {
        std::string dev = "/dev/" + sr;
        json media = {{"state", "none"}, {"class_cd", true}, {"readable", false}};
        int fd = open(dev.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            media["state"] = mediaState(fd);
            if (media["state"] == "present") {
                int ds = ioctl(fd, CDROM_DISC_STATUS, 0);
                media["type"] = ds == CDS_AUDIO ? "CD-DA" : ds == CDS_MIXED ? "CD (mixed)" : "CD-ROM";
                media["class_cd"] = ds == CDS_AUDIO || ds == CDS_MIXED || ds == CDS_DATA_1 || ds == CDS_DATA_2 ||
                                    ds == CDS_XA_2_1 || ds == CDS_XA_2_2;
                media["readable"] = ds == CDS_AUDIO || ds == CDS_MIXED;
            }
            close(fd);
        }
        out.push_back({{"id", dev},
                       {"vendor", readSysfs("/sys/class/block/" + sr + "/device/vendor")},
                       {"product", readSysfs("/sys/class/block/" + sr + "/device/model")},
                       {"firmware", readSysfs("/sys/class/block/" + sr + "/device/rev")},
                       {"name", driveName(sr)},
                       {"can_read", true},
                       {"media", media}});
    }
    return {{"platform", "linux"}, {"drives", out}};
}

std::unique_ptr<cd::Reader> platformOpenReader(const std::string &device) {
    std::string dev = pickDevice(device);
    int fd = openDrive(dev);
    std::string state = mediaState(fd);
    if (state != "present") {
        close(fd);
        fail(FISO_E_UNSUPPORTED, "[no_media] no disc in the drive");
    }
    auto reader = std::make_unique<LinuxReader>(driveName(dev.substr(5)), fd);
    reader->checkAudioDisc();
    return reader;
}

void platformEject(const std::string &device) {
    std::string dev = pickDevice(device);
    int fd = openDrive(dev);
    ioctl(fd, CDROM_LOCKDOOR, 0);
    int r = ioctl(fd, CDROMEJECT, 0);
    int err = errno;
    close(fd);
    if (r != 0) {
        if (err == EBUSY) fail(FISO_E_IO, "[busy] the disc is used by another application");
        fail(FISO_E_IO, std::string("eject failed: ") + strerror(err));
    }
}

}  // namespace fiso
#endif  // __linux__
