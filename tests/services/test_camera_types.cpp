// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the camera format tables in camera_types.cpp: the names the
// luna API uses for a format, the codes behind them, and the bit flags the
// codes are made of.
//
// NV21 has its own entry because a camera that produces it (the droid plugin
// does) used to be reported as "YUV", which every client reads as packed
// YUYV, and the frames came out as garbage.

#include <gtest/gtest.h>

#include "camera_types.h"

#include <cstring>
#include <string>

namespace
{

const camera_format_t kAllFormats[] = {CAMERA_FORMAT_YUV, CAMERA_FORMAT_NV21, CAMERA_FORMAT_H264ES,
                                       CAMERA_FORMAT_JPEG};

camera_format_t parse(const std::string &name)
{
    camera_format_t code = CAMERA_FORMAT_YUV;
    convertFormatToCode(name, &code);
    return code;
}

} // namespace

TEST(CameraTypesFormat, NamesOfEveryFormat)
{
    EXPECT_EQ("YUV", getFormatStringFromCode(CAMERA_FORMAT_YUV));
    EXPECT_EQ("NV21", getFormatStringFromCode(CAMERA_FORMAT_NV21));
    EXPECT_EQ("H264ES", getFormatStringFromCode(CAMERA_FORMAT_H264ES));
    EXPECT_EQ("JPEG", getFormatStringFromCode(CAMERA_FORMAT_JPEG));
}

TEST(CameraTypesFormat, ParsingEveryName)
{
    EXPECT_EQ(CAMERA_FORMAT_YUV, parse("YUV"));
    EXPECT_EQ(CAMERA_FORMAT_NV21, parse("NV21"));
    EXPECT_EQ(CAMERA_FORMAT_H264ES, parse("H264ES"));
    EXPECT_EQ(CAMERA_FORMAT_JPEG, parse("JPEG"));
}

TEST(CameraTypesFormat, UnknownNamesAreUndefined)
{
    EXPECT_EQ(CAMERA_FORMAT_UNDEFINED, parse(""));
    EXPECT_EQ(CAMERA_FORMAT_UNDEFINED, parse("NV12"));
    EXPECT_EQ(CAMERA_FORMAT_UNDEFINED, parse("I420"));
    // Matching is exact: the API has always been case sensitive.
    EXPECT_EQ(CAMERA_FORMAT_UNDEFINED, parse("nv21"));
}

// getInfo reports each format under the key getResolutionString() gives it,
// and camera2 reads that key back with convertFormatToCode(). A format whose
// key does not parse back is dropped from the device's format list, which is
// how an unnamed format ends up as an empty key.
TEST(CameraTypesFormat, ResolutionKeyRoundTrips)
{
    for (camera_format_t code : kAllFormats)
    {
        const std::string key = getResolutionString(code);
        EXPECT_FALSE(key.empty()) << "format " << code << " has no key";
        EXPECT_EQ(code, parse(key)) << "key " << key;
        EXPECT_EQ(getFormatStringFromCode(code), key);
    }
}

TEST(CameraTypesFormat, UndefinedHasNoResolutionKey)
{
    EXPECT_TRUE(getResolutionString(CAMERA_FORMAT_UNDEFINED).empty());
}

// camera_format_t values are bit flags, and getFormatString() decodes a mask
// of them, so every format needs a bit of its own.
TEST(CameraTypesFormat, FormatsAreDistinctBitFlags)
{
    int seen = 0;
    for (camera_format_t code : kAllFormats)
    {
        ASSERT_GT(code, 0);
        EXPECT_EQ(0, code & (code - 1)) << "format " << code << " is not a single bit";
        EXPECT_EQ(0, seen & code) << "format " << code << " shares a bit";
        seen |= code;
    }
}

TEST(CameraTypesFormat, MaskDecodesEveryFormat)
{
    char names[100];

    getFormatString(CAMERA_FORMAT_NV21, names);
    EXPECT_STREQ("NV21|", names);

    getFormatString(CAMERA_FORMAT_YUV | CAMERA_FORMAT_NV21, names);
    EXPECT_NE(nullptr, strstr(names, "YUV|"));
    EXPECT_NE(nullptr, strstr(names, "NV21|"));
    EXPECT_EQ(nullptr, strstr(names, "JPEG|"));

    int all = 0;
    for (camera_format_t code : kAllFormats)
        all |= code;
    getFormatString(all, names);
    for (const char *name : {"YUV|", "NV21|", "H264ES|", "JPEG|"})
        EXPECT_NE(nullptr, strstr(names, name)) << name;
}

// The facing a camera reports in getInfo(). A camera that does not know which
// way it faces reports nothing, and a client that does not recognise what it
// reads is left with UNKNOWN too, so the two ends can be updated separately.
TEST(CameraTypesFacing, NamesOfEveryFacing)
{
    EXPECT_EQ("front", getFacingString(CAMERA_FACING_FRONT));
    EXPECT_EQ("back", getFacingString(CAMERA_FACING_BACK));
}

TEST(CameraTypesFacing, UnknownFacingReportsNothing)
{
    EXPECT_TRUE(getFacingString(CAMERA_FACING_UNKNOWN).empty());
}

TEST(CameraTypesFacing, ParsingEveryName)
{
    EXPECT_EQ(CAMERA_FACING_FRONT, convertFacingToCode("front"));
    EXPECT_EQ(CAMERA_FACING_BACK, convertFacingToCode("back"));
}

TEST(CameraTypesFacing, UnrecognisedNamesAreUnknown)
{
    EXPECT_EQ(CAMERA_FACING_UNKNOWN, convertFacingToCode(""));
    EXPECT_EQ(CAMERA_FACING_UNKNOWN, convertFacingToCode("rear"));
    EXPECT_EQ(CAMERA_FACING_UNKNOWN, convertFacingToCode("external"));
    // Matching is exact, like the format names.
    EXPECT_EQ(CAMERA_FACING_UNKNOWN, convertFacingToCode("Front"));
}

TEST(CameraTypesFacing, NameRoundTrips)
{
    for (camera_facing_t facing : {CAMERA_FACING_FRONT, CAMERA_FACING_BACK})
        EXPECT_EQ(facing, convertFacingToCode(getFacingString(facing)));
    EXPECT_EQ(CAMERA_FACING_UNKNOWN, convertFacingToCode(getFacingString(CAMERA_FACING_UNKNOWN)));
}

// A device info that nobody filled in must not claim to face anywhere.
TEST(CameraTypesFacing, DeviceInfoDefaultsToUnknown)
{
    camera_device_info_t info;
    EXPECT_EQ(CAMERA_FACING_UNKNOWN, info.n_facing);
}
