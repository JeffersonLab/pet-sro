package org.jlab.ersap.actor.util;
/**
 * Copyright (c) 2021, Jefferson Science Associates, all rights reserved.
 * See LICENSE.txt file.
 * Thomas Jefferson National Accelerator Facility
 * Experimental Physics Software and Computing Infrastructure Group
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 * @author gurjyan on 12/10/24
 * @project pet-sro
 */

public final class EConstants {
    // Prevents instantiation
    private EConstants() {
        throw new UnsupportedOperationException("This is a utility class and cannot be instantiated");
    }

    // ERSAP-wide constants
    public static final String udf = "undefined";
}
