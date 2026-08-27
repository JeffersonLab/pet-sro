package org.jlab.ersap.actor.pet.engine;
/**
 * Copyright (c) 2021, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 * @author gurjyan on 8/26/26
 * @project pet-sro
 */

import org.jlab.epsci.ersap.base.ErsapUtil;
import org.jlab.epsci.ersap.engine.Engine;
import org.jlab.epsci.ersap.engine.EngineData;
import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.epsci.ersap.engine.EngineStatus;
import org.jlab.ersap.actor.datatypes.EvioBlockDataType;
import org.jlab.ersap.actor.datatypes.JavaObjectType;
import org.json.JSONObject;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Set;

/**
 * Prints the frame number and the timestamp of every EVIO block published by
 * the C++ {@code EjfatReceiverActor}.
 *
 * <p>The payload is one complete big-endian EVIO version-4 block, block-length
 * word included. The fields printed here are the FEB SRO ones, at the offsets
 * defined by {@code cpp/include/SroWireFormat.hpp}:
 *
 * <pre>
 *   word  0   EVIO block length in 32-bit words, including this word
 *   word  7   EVIO magic, 0xC0DA0100
 *   word  9   event bank tag/type -- rocid in the upper 16 bits
 *   word 13   frame counter, unsigned 32-bit, starts at 0
 *   word 14   timestamp, low  32 bits
 *   word 15   timestamp, high 32 bits
 * </pre>
 *
 * <p>The timestamp is in nanoseconds and advances by one frame period
 * (1 ms as the firmware is configured) per frame.
 *
 * <p>The block is passed downstream unchanged, so this engine can be dropped
 * into a chain without disturbing it.
 */
public class PetSinglesProcessorEngine implements Engine {

    // Word offsets, in bytes. See SroWireFormat.hpp.
    private static final int OFF_BLOCK_LENGTH = 0 * 4;
    private static final int OFF_MAGIC = 7 * 4;
    private static final int OFF_BANK_TAG = 9 * 4;
    private static final int OFF_FRAME_COUNTER = 13 * 4;
    private static final int OFF_TIMESTAMP_LO = 14 * 4;
    private static final int OFF_TIMESTAMP_HI = 15 * 4;

    /** Words 0..15 must be present before any field above can be read. */
    private static final int MIN_BLOCK_BYTES = 16 * 4;

    private static final int EVIO_MAGIC = 0xC0DA0100;

    /** Print every n-th block. 1, the default, prints every one. */
    private int frequency = 1;

    private long eventCount = 0;
    private long malformedCount = 0;

    @Override
    public EngineData configure(EngineData engineData) {
        if (engineData.getMimeType().equalsIgnoreCase(EngineDataType.JSON.mimeType())) {
            JSONObject data = new JSONObject((String) engineData.getData());
            if (data.has("frequency")) {
                int f = data.getInt("frequency");
                if (f < 1) {
                    EngineData error = new EngineData();
                    error.setStatus(EngineStatus.ERROR);
                    error.setDescription("PetSinglesProcessorEngine: frequency must be >= 1");
                    return error;
                }
                frequency = f;
            }
        }
        eventCount = 0;
        malformedCount = 0;
        return null;
    }

    @Override
    public EngineData execute(EngineData engineData) {
        EngineData out = new EngineData();

        String mimeType = engineData.getMimeType();
        if (!mimeType.equals(EvioBlockDataType.MIME_TYPE)
                && !mimeType.equals(JavaObjectType.JOBJ.mimeType())) {
            out.setStatus(EngineStatus.ERROR);
            out.setDescription("PetSinglesProcessorEngine: expected "
                    + EvioBlockDataType.MIME_TYPE + " or " + JavaObjectType.JOBJ.mimeType()
                    + ", got " + mimeType);
            return out;
        }

        Object data = engineData.getData();
        if (!(data instanceof ByteBuffer)) {
            out.setStatus(EngineStatus.ERROR);
            out.setDescription("PetSinglesProcessorEngine: expected a ByteBuffer, got "
                    + (data == null ? "null" : data.getClass().getName()));
            return out;
        }

        // duplicate() so the downstream view keeps its own position and order.
        // Forcing BIG_ENDIAN here means this engine reads correctly whether the
        // block arrived as binary/data-evio (already big-endian) or as
        // binary/data-jobj (which ERSAP marks little-endian for a C++ producer,
        // because xMsgMeta.byteOrder defaults to Little when unset).
        ByteBuffer buf = ((ByteBuffer) data).duplicate().order(ByteOrder.BIG_ENDIAN);
        int base = buf.position();
        int size = buf.remaining();

        String problem = validate(buf, base, size);
        if (problem != null) {
            malformedCount++;
            out.setStatus(EngineStatus.WARNING);
            out.setDescription("PetSinglesProcessorEngine: " + problem);
            System.err.println("PetSinglesProcessorEngine: malformed block #"
                    + (eventCount + 1) + ": " + problem);
            // Still pass it on: dropping data is the sink's decision, not this
            // engine's, and it only prints.
            out.setData(engineData.getMimeType(), data);
            eventCount++;
            return out;
        }

        long frameNumber = Integer.toUnsignedLong(buf.getInt(base + OFF_FRAME_COUNTER));
        long tsLo = Integer.toUnsignedLong(buf.getInt(base + OFF_TIMESTAMP_LO));
        long tsHi = Integer.toUnsignedLong(buf.getInt(base + OFF_TIMESTAMP_HI));
        long timestamp = (tsHi << 32) | tsLo;
        int rocid = (buf.getInt(base + OFF_BANK_TAG) >>> 16) & 0xFFFF;

        eventCount++;
        if (eventCount % frequency == 0) {
            // getCommunicationId() is the EJFAT event number the C++ actor set.
            // xMsgMeta carries it as a fixed32, so it wraps at 2^32.
            System.out.printf(
                    "PET singles: frame_number=%d timestamp=%d ns (%.6f s) "
                            + "rocid=%d bytes=%d ejfat_event=%d%n",
                    frameNumber, timestamp, timestamp / 1.0e9, rocid, size,
                    Integer.toUnsignedLong(engineData.getCommunicationId()));
        }

        // Pass the block downstream untouched, under the type it arrived as.
        out.setData(engineData.getMimeType(), data);
        return out;
    }

    /**
     * Returns null when the buffer holds a usable EVIO block, otherwise a
     * message naming the first problem. Reads nothing outside the buffer.
     */
    private static String validate(ByteBuffer buf, int base, int size) {
        if (size < MIN_BLOCK_BYTES) {
            return size + " bytes is shorter than the " + MIN_BLOCK_BYTES
                    + " needed for an EVIO header with a timestamp";
        }
        int magic = buf.getInt(base + OFF_MAGIC);
        if (magic != EVIO_MAGIC) {
            return String.format("bad EVIO magic 0x%08X, expected 0x%08X", magic, EVIO_MAGIC);
        }
        long declaredBytes = Integer.toUnsignedLong(buf.getInt(base + OFF_BLOCK_LENGTH)) * 4L;
        if (declaredBytes != size) {
            return "block declares " + declaredBytes + " bytes but " + size + " were delivered";
        }
        return null;
    }

    @Override
    public EngineData executeGroup(Set<EngineData> set) {
        return null;
    }

    @Override
    public Set<EngineDataType> getInputDataTypes() {
        return ErsapUtil.buildDataTypes(EvioBlockDataType.EVIO_BLOCK,
                JavaObjectType.JOBJ,
                EngineDataType.JSON);
    }

    @Override
    public Set<EngineDataType> getOutputDataTypes() {
        return ErsapUtil.buildDataTypes(EvioBlockDataType.EVIO_BLOCK,
                JavaObjectType.JOBJ);
    }

    @Override
    public Set<String> getStates() {
        return null;
    }

    @Override
    public String getDescription() {
        return "Prints the frame number and timestamp of every EVIO block "
                + "received from the EJFAT receiver actor";
    }

    @Override
    public String getVersion() {
        return "v1.0";
    }

    @Override
    public String getAuthor() {
        return "gurjyan";
    }

    @Override
    public void reset() {
        eventCount = 0;
        malformedCount = 0;
    }

    @Override
    public void destroy() {
        System.out.println("PetSinglesProcessorEngine: " + eventCount
                + " block(s) seen, " + malformedCount + " malformed");
    }
}
