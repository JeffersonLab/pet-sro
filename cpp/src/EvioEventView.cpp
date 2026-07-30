#include "EvioEventView.hpp"

#include "SroWireFormat.hpp"

#include <sstream>

namespace petsro {

EvioEventView inspectEvioEvent(const std::uint8_t* data, std::size_t length) noexcept {
    EvioEventView view;

    try {
        if (data == nullptr) {
            view.problem = "null event buffer";
            return view;
        }

        // One bounds test covers every field read below: the block must be long
        // enough to contain word 15, the timestamp high word.
        const std::size_t minBytes = sro::wordOffset(sro::MIN_TIMESTAMPED_BLOCK_WORDS);
        if (length < minBytes) {
            std::ostringstream oss;
            oss << "event of " << length << " bytes is shorter than the " << minBytes
                << " needed for an EVIO header with a timestamp";
            view.problem = oss.str();
            return view;
        }

        view.blockWords = sro::readBe32(data + sro::wordOffset(sro::WORD_BLOCK_LENGTH));

        const std::uint32_t magic = sro::readBe32(data + sro::wordOffset(sro::WORD_MAGIC));
        if (magic != sro::EVIO_MAGIC) {
            std::ostringstream oss;
            oss << "bad EVIO magic 0x" << std::hex << magic;
            view.problem = oss.str();
            return view;
        }

        // The reader cannot get this wrong -- it sized the buffer from word 0 --
        // but the network can: a short or over-long delivery shows up here and
        // nowhere else.
        const std::size_t declaredBytes = static_cast<std::size_t>(view.blockWords) * 4U;
        if (declaredBytes != length) {
            std::ostringstream oss;
            oss << "block declares " << view.blockWords << " words (" << declaredBytes
                << " bytes) but " << length << " bytes were delivered";
            view.problem = oss.str();
            return view;
        }

        view.timestamp = sro::combineTimestamp(
            sro::readBe32(data + sro::wordOffset(sro::WORD_TIMESTAMP_LO)),
            sro::readBe32(data + sro::wordOffset(sro::WORD_TIMESTAMP_HI)));
        view.frameCounter = sro::readBe32(data + sro::wordOffset(sro::WORD_FRAME_COUNTER));
        view.rocid =
            sro::rocidFromBankTag(sro::readBe32(data + sro::wordOffset(sro::WORD_BANK_TAG)));

        view.valid = true;
        return view;
    } catch (...) {
        // Declared noexcept, and the only thing above that can throw is the
        // ostringstream on an allocation failure. Report rather than terminate.
        view.valid = false;
        return view;
    }
}

}  // namespace petsro
