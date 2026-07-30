// EvioFileReader.hpp -- streaming reader for a single FEB SRO capture file.
//
// Reimplements, in streaming form, the file handling of
//   org.jlab.detimg.petiroc.replay.ReplayFrameSource
// The Java class maps or normalises the whole capture up front because it
// serves thousands of emulated streams from one copy. This program replays a
// handful of files exactly once per loop, so it reads block by block instead
// and never holds more than one event in memory per file.

#ifndef PETSRO_EVIOFILEREADER_HPP
#define PETSRO_EVIOFILEREADER_HPP

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace petsro {

/// Outcome of an attempt to read the next event.
enum class ReadStatus {
    Ok,          ///< An event was produced.
    EndOfFile,   ///< Input is exhausted. Not an error.
    Malformed,   ///< The bytes are not a usable EVIO block. Replay must stop.
    IoError      ///< The underlying stream failed. Replay must stop.
};

const char* toString(ReadStatus status) noexcept;

/// One EVIO block plus the fields decoded out of its header.
///
/// `data` always holds the block in normalised BIG-ENDIAN wire form with the
/// block-length word present, whichever on-disk format it came from, so it can
/// be handed to the packetizer untouched.
struct EvioEvent {
    std::vector<std::uint8_t> data;
    std::uint64_t timestamp = 0;     ///< nanoseconds, from words 14/15
    std::uint64_t frameCounter = 0;  ///< word 13, unsigned 32-bit widened
    std::uint32_t rocid = 0;         ///< word 9, upper 16 bits

    void clear() noexcept {
        data.clear();  // keeps capacity, so the next read reuses the allocation
        timestamp = 0;
        frameCounter = 0;
        rocid = 0;
    }
};

/// Anything the synchronizer can pull events from. Exists so the
/// synchronization logic can be tested against synthetic streams with no file
/// system involved.
class EventSource {
  public:
    virtual ~EventSource() = default;

    /// Produces the next event. On ReadStatus::Ok, `out` is overwritten; on
    /// any other status `out` is left in an unspecified but valid state.
    virtual ReadStatus next(EvioEvent& out) = 0;

    /// Human-readable identity for logs and error messages.
    virtual const std::string& name() const noexcept = 0;
};

/// The two capture formats ReplayFrameSource accepts, detected the same way it
/// detects them: by the byte order of the EVIO magic at byte offset 28.
enum class EvioFormat {
    Unknown,
    /// evio_<ip>.bin -- raw big-endian wire dump, block-length word present.
    WireDump,
    /// pet_sro_feb_stream<N>.bin -- little-endian, length-prefixed, block-length
    /// word stripped by the C writer. Normalised to wire form on read.
    FebStreamDump
};

const char* toString(EvioFormat format) noexcept;

/// Per-file counters, reported at shutdown.
struct ReaderStats {
    std::uint64_t eventsRead = 0;
    std::uint64_t eventsSkipped = 0;  ///< dropped by the synchronizer
    std::uint64_t bytesRead = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t truncatedTails = 0;  ///< incomplete final block, per open
    std::uint64_t opens = 0;
};

/// Opens one capture file and walks its EVIO blocks.
///
/// All resources are owned by std::ifstream and std::vector, so destruction is
/// sufficient cleanup and no explicit close is required on an error path.
class EvioFileReader final : public EventSource {
  public:
    explicit EvioFileReader(std::string path);

    EvioFileReader(const EvioFileReader&) = delete;
    EvioFileReader& operator=(const EvioFileReader&) = delete;

    /// Opens the file and detects its format. Returns false and sets
    /// lastError() if the file cannot be opened or is not a capture.
    bool open();

    /// Closes and reopens from byte zero. Format is re-detected, because a
    /// replay set could in principle be swapped underneath us between loops.
    bool reopen();

    void close() noexcept;

    ReadStatus next(EvioEvent& out) override;

    const std::string& name() const noexcept override { return path_; }

    const std::string& path() const noexcept { return path_; }
    EvioFormat format() const noexcept { return format_; }
    bool isOpen() const noexcept { return stream_.is_open(); }

    /// Description of the most recent failure, for error reporting.
    const std::string& lastError() const noexcept { return lastError_; }

    const ReaderStats& stats() const noexcept { return stats_; }
    ReaderStats& mutableStats() noexcept { return stats_; }

  private:
    /// Reads exactly `count` bytes into `dst`. Returns the number actually
    /// read; a short count at a clean boundary means end of file.
    std::size_t readExactly(std::uint8_t* dst, std::size_t count);

    /// Reads the first 32 bytes and classifies the file.
    bool detectFormat();

    ReadStatus nextWireDump(EvioEvent& out);
    ReadStatus nextFebStreamDump(EvioEvent& out);

    /// Validates magic and block length, then fills timestamp/rocid/counter.
    /// `out.data` must already hold a normalised big-endian block.
    ReadStatus decodeHeader(EvioEvent& out, std::uint64_t blockStart);

    ReadStatus fail(ReadStatus status, const std::string& message);

    std::string path_;
    std::ifstream stream_;
    EvioFormat format_ = EvioFormat::Unknown;
    std::string lastError_;
    ReaderStats stats_;

    /// Byte offset of the block currently being read, for error messages.
    std::uint64_t offset_ = 0;
};

}  // namespace petsro

#endif  // PETSRO_EVIOFILEREADER_HPP
