package org.jlab.ersap.actor.datatypes;
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

import org.jlab.epsci.ersap.base.error.ErsapException;
import org.jlab.epsci.ersap.engine.EngineDataType;
import org.jlab.epsci.ersap.engine.ErsapSerializer;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * One complete EVIO block, carried as raw bytes.
 *
 * <p>This is the type the C++ {@code EjfatReceiverActor} publishes: the payload
 * is exactly one big-endian EVIO version-4 block as it came off the EJFAT
 * reassembler, block-length word included. There is no length prefix, no
 * envelope and no metadata in the payload -- the EJFAT event number travels in
 * the ERSAP communication id instead.
 *
 * <p><b>Why this exists instead of {@link EngineDataType#BYTES}.</b> ERSAP
 * carries the byte order in {@code xMsgMeta.byteOrder}, a proto2 {@code
 * optional} with no explicit default over an enum whose first constant is
 * {@code Little = 1}. An unset field therefore reads back as {@code Little},
 * and ersap-cpp never sets it, so {@code DataUtil.deserialize()} stamps every
 * buffer published by a C++ actor {@code LITTLE_ENDIAN}. The bytes are correct
 * and complete; only the order flag is wrong, and the C++ side cannot fix it
 * because {@code EngineData}'s metadata is private to the framework.
 *
 * <p>So the correction lives here, in the deserializer, where no processing
 * actor can forget it. A consumer receives a {@link ByteBuffer} that is
 * already {@link ByteOrder#BIG_ENDIAN}, positioned at zero, with
 * {@code limit == capacity == payload length}.
 *
 * <p>The serializer forces the same order on the way out, so a Java producer
 * publishing this type always stamps the metadata {@code Big} and the round
 * trip is stable in both directions.
 */
public final class EvioBlockDataType {

    public static final String MIME_TYPE = "binary/data-evio";

    private EvioBlockDataType() { }

    private static class EvioBlockSerializer implements ErsapSerializer {

        @Override
        public ByteBuffer write(Object data) throws ErsapException {
            if (!(data instanceof ByteBuffer)) {
                throw new ErsapException("Expected ByteBuffer, got "
                        + (data == null ? "null" : data.getClass().getName()));
            }
            // duplicate() so the caller's buffer keeps its own order and
            // position; it shares the backing array, which is what
            // DataUtil.serialize() reads with array().
            return ((ByteBuffer) data).duplicate().order(ByteOrder.BIG_ENDIAN);
        }

        @Override
        public Object read(ByteBuffer buffer) throws ErsapException {
            // The buffer was created by DataUtil.deserialize() for this call
            // alone, so setting the order in place is safe.
            return buffer.order(ByteOrder.BIG_ENDIAN);
        }
    }

    public static final EngineDataType EVIO_BLOCK =
            new EngineDataType(MIME_TYPE, new EvioBlockSerializer());
}
