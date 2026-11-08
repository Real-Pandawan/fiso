// CD reading with standard MMC commands (SCSI Multi-Media Commands), shared by the platform
// adapters that can send commands to the drive (Linux SG_IO). The data returned matches the
// cd::Reader contract: little-endian CD-DA, 294-byte C2 error pointers, 12-byte Q + padding.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "cd.h"

namespace fiso::cd {

// Sends one command; returns the number of bytes received, or -1 when the drive reports an error.
class MmcTransport {
  public:
    virtual ~MmcTransport() = default;
    virtual long command(const uint8_t *cdb, size_t cdbLen, uint8_t *data, size_t dataLen) = 0;
};

// Rebuilds the 12 bytes of sub-channel Q (control/ADR … CRC) from 96 bytes of raw P-W
// sub-channel data, where bit 6 of every byte carries one bit of Q.
void qFromRawPW(const uint8_t *pw96, uint8_t *q16);

class MmcReader : public Reader {
  public:
    // The transport must outlive the reader.
    MmcReader(std::string name, MmcTransport &transport) : name_(std::move(name)), t_(transport) {}

    // Fails with [no_media] when no TOC can be read and with [no_audio] when the disc has no audio track.
    void checkAudioDisc();

    std::string driveName() override { return name_; }
    DiscToc toc() override;
    bool hasC2() override;
    bool read(int32_t lba, int n, uint8_t *audio, uint8_t *c2) override;
    bool readQ(int32_t lba, uint8_t *q16) override;
    std::string isrc(int track) override;
    std::string mcn() override;
    std::vector<uint8_t> cdText() override;

  private:
    std::vector<uint8_t> readTocFormat(uint8_t format, bool msf, uint8_t session);
    bool readCd(int32_t lba, int n, uint8_t flags, uint8_t subchannel, uint8_t *out, size_t bytes);

    std::string name_;
    MmcTransport &t_;
    int c2_ = -1;  // unknown, 0 = unsupported, 1 = supported
    std::vector<uint8_t> buf_;
};

}  // namespace fiso::cd
