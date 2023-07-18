#include <yt/yt/client/table_client/columnar_statistics.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/unversioned_value.h>

#include <yt/yt/client/table_client/unittests/helpers/helpers.h>

#include <yt/yt/core/test_framework/framework.h>

namespace NYT::NTableClient {
namespace {

std::vector<TUnversionedRow> CaptureRows(TRowBufferPtr rowBuffer, const std::vector<std::vector<TUnversionedValue>>& rows)
{
    std::vector<TUnversionedRow> result(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        result[i] = rowBuffer->CaptureRow(rows[i]);
    }
    return result;
}

TEST(TUpdateColumnarStatisticsTest, EmptyStruct)
{
    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {
            MakeUnversionedInt64Value(12, 0),
            MakeUnversionedStringValue("buzz", 1),
            MakeUnversionedDoubleValue(1e70, 2),
        },
        {
            MakeUnversionedInt64Value(-7338, 0),
            MakeUnversionedStringValue("foo", 1),
            MakeUnversionedDoubleValue(-0.16, 2),
        },
        {
            MakeUnversionedNullValue(0),
            MakeUnversionedStringValue("chyt", 1),
            MakeUnversionedDoubleValue(-std::numeric_limits<double>::infinity(), 2),
        },
    });

    TColumnarStatistics statistics = TColumnarStatistics::MakeEmpty(3);
    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {16, 11, 24},
        .ColumnMinValues = {
            MakeUnversionedInt64Value(-7338),
            MakeUnversionedStringValue("buzz"),
            MakeUnversionedDoubleValue(-std::numeric_limits<double>::infinity()),
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(12),
            MakeUnversionedStringValue("foo"),
            MakeUnversionedDoubleValue(1e70),
        },
        .ColumnNonNullValueCounts = {2, 3, 3},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, InitializedStruct)
{
    TColumnarStatistics statistics;
    statistics.ColumnMinValues = {
        MakeUnversionedUint64Value(5),
        MakeUnversionedStringValue("gaga"),
        MakeUnversionedInt64Value(-77777),
        MakeUnversionedSentinelValue(EValueType::Min),
    };
    statistics.ColumnMaxValues = {
        MakeUnversionedUint64Value(500),
        MakeUnversionedStringValue("gugu"),
        MakeUnversionedInt64Value(10),
        MakeUnversionedSentinelValue(EValueType::Max),
    };
    statistics.ColumnNonNullValueCounts = {
        5,
        10,
        50,
        3
    };
    statistics.ColumnDataWeights = {
        40,
        80,
        400,
        128,
    };

    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {
            MakeUnversionedUint64Value(2, 0),
            MakeUnversionedStringValue("blabla", 1),
            MakeUnversionedAnyValue("[1,2,3]", 2),
            MakeUnversionedInt64Value(2, 3),
        },
        {
            MakeUnversionedUint64Value(22, 0),
            MakeUnversionedStringValue("radio", 1),
            MakeUnversionedInt64Value(29292, 2),
            MakeUnversionedInt64Value(-10000, 3),
        },
    });

    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {56, 91, 415, 144},
        .ColumnMinValues = {
            MakeUnversionedUint64Value(2),
            MakeUnversionedStringValue("blabla"),
            MakeUnversionedSentinelValue(EValueType::Min),
            MakeUnversionedSentinelValue(EValueType::Min),
        },
        .ColumnMaxValues = {
            MakeUnversionedUint64Value(500),
            MakeUnversionedStringValue("radio"),
            MakeUnversionedSentinelValue(EValueType::Max),
            MakeUnversionedSentinelValue(EValueType::Max),
        },
        .ColumnNonNullValueCounts = {7, 12, 52, 5},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, DefaultStruct)
{
    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {
            MakeUnversionedInt64Value(12, 0),
            MakeUnversionedStringValue("buzz", 1),
            MakeUnversionedDoubleValue(1e70, 2),
        },
    });

    TColumnarStatistics statistics;
    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {8, 4, 8},
        .ColumnMinValues = {
            MakeUnversionedInt64Value(12),
            MakeUnversionedStringValue("buzz"),
            MakeUnversionedDoubleValue(1e70),
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(12),
            MakeUnversionedStringValue("buzz"),
            MakeUnversionedDoubleValue(1e70),
        },
        .ColumnNonNullValueCounts = {1, 1, 1},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, StructSizeLessThanRowSize)
{
    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {
            MakeUnversionedInt64Value(12, 0),
            MakeUnversionedStringValue("buzz", 1),
            MakeUnversionedDoubleValue(1e70, 2),
        },
    });

    TColumnarStatistics statistics = TColumnarStatistics::MakeEmpty(1);
    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {8, 4, 8},
        .ColumnMinValues = {
            MakeUnversionedInt64Value(12),
            MakeUnversionedStringValue("buzz"),
            MakeUnversionedDoubleValue(1e70),
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(12),
            MakeUnversionedStringValue("buzz"),
            MakeUnversionedDoubleValue(1e70),
        },
        .ColumnNonNullValueCounts = {1, 1, 1},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, NoValueStatistics)
{
    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {
            MakeUnversionedInt64Value(12, 0),
            MakeUnversionedStringValue("buzz", 1),
            MakeUnversionedDoubleValue(1e70, 2),
        },
    });

    TColumnarStatistics statistics = TColumnarStatistics::MakeEmpty(3, /*hasValueStatistics*/ false);
    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {8, 4, 8},
        .ColumnMinValues = {},
        .ColumnMaxValues = {},
        .ColumnNonNullValueCounts = {},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, LegacyStruct)
{
    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {
            MakeUnversionedInt64Value(12, 0),
            MakeUnversionedStringValue("buzz", 1),
            MakeUnversionedDoubleValue(1e70, 2),
        },
    });

    TColumnarStatistics statistics = TColumnarStatistics::MakeLegacy(3, 82);
    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {8, 4, 8},
        .LegacyChunkDataWeight = 82,
        .ColumnMinValues = {},
        .ColumnMaxValues = {},
        .ColumnNonNullValueCounts = {},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, CheckStringApproximation)
{
    constexpr int MaxStringValueLength = 100;

    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {MakeUnversionedStringValue(std::string(MaxStringValueLength + 1, 'c'))},
        {MakeUnversionedStringValue("cb")},
        {MakeUnversionedStringValue(std::string(70, 'a') + std::string(MaxStringValueLength - 70 + 1, 'x'))},
    });

    TColumnarStatistics statistics = TColumnarStatistics::MakeEmpty(1);
    statistics.Update(rows);

    std::vector<TUnversionedOwningValue> expectedColumnMinValues =
        {MakeUnversionedStringValue(std::string(70, 'a') + std::string(MaxStringValueLength - 70, 'x'))};
    std::vector<TUnversionedOwningValue> expectedColumnMaxValues =
        {MakeUnversionedStringValue(std::string(MaxStringValueLength - 1, 'c') + "d")};
    EXPECT_EQ(statistics.ColumnMinValues, expectedColumnMinValues);
    EXPECT_EQ(statistics.ColumnMaxValues, expectedColumnMaxValues);
}

TEST(TUpdateColumnarStatisticsTest, NullColumn)
{
    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {MakeUnversionedNullValue()},
        {MakeUnversionedNullValue()},
        {MakeUnversionedNullValue()},
    });

    TColumnarStatistics statistics = TColumnarStatistics::MakeEmpty(1);
    statistics.Update(rows);

    TColumnarStatistics expected = TColumnarStatistics::MakeEmpty(1);
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, DifferentTypesInOneColumn)
{
    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows = CaptureRows(rowBuffer, {
        {MakeUnversionedInt64Value(1000)},
        {MakeUnversionedStringValue("chyt")},
        {MakeUnversionedBooleanValue(true)},
    });

    TColumnarStatistics statistics = TColumnarStatistics::MakeEmpty(1);
    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {13},
        .ColumnMinValues = {MakeUnversionedInt64Value(1000)},
        .ColumnMaxValues = {MakeUnversionedStringValue("chyt")},
        .ColumnNonNullValueCounts = {3},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TUpdateColumnarStatisticsTest, VersionedRow)
{
    std::vector<TVersionedRow> rows;
    TRowBufferPtr buffer = New<TRowBuffer>();
    TVersionedRowBuilder builder(buffer);

    builder.AddKey(MakeUnversionedInt64Value(12));
    builder.AddValue(MakeVersionedStringValue("b", 10, 1));
    builder.AddValue(MakeVersionedInt64Value(1, 11, 2));
    rows.emplace_back(builder.FinishRow());

    builder.AddKey(MakeUnversionedInt64Value(33));
    builder.AddValue(MakeVersionedStringValue("c", 15, 1));
    builder.AddValue(MakeVersionedStringValue("a", 20, 1));
    builder.AddValue(MakeVersionedInt64Value(143, 11, 2));
    rows.emplace_back(builder.FinishRow());

    TColumnarStatistics statistics = TColumnarStatistics::MakeEmpty(3);
    statistics.Update(rows);

    TColumnarStatistics expected{
        .ColumnDataWeights = {16, 3 + 3 * sizeof(TTimestamp), 16 + 2 * sizeof(TTimestamp)},
        .TimestampTotalWeight = 40,
        .ColumnMinValues = {
            MakeUnversionedInt64Value(12),
            MakeUnversionedStringValue("a"),
            MakeUnversionedInt64Value(1)
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(33),
            MakeUnversionedStringValue("c"),
            MakeUnversionedInt64Value(143)
        },
        .ColumnNonNullValueCounts = {2, 3, 2},
    };
    EXPECT_EQ(statistics, expected);
}

TEST(TMergeColumnarStatisticsTest, EmptyAndNonEmpty)
{
    TColumnarStatistics lhs = TColumnarStatistics::MakeEmpty(4);
    TColumnarStatistics rhs{
        .ColumnDataWeights = {40, 80, 400, 128},
        .ColumnMinValues = {
            MakeUnversionedUint64Value(5),
            MakeUnversionedStringValue("gaga"),
            MakeUnversionedInt64Value(-77777),
            MakeUnversionedSentinelValue(EValueType::Min),
        },
        .ColumnMaxValues = {
            MakeUnversionedUint64Value(500),
            MakeUnversionedStringValue("gugu"),
            MakeUnversionedInt64Value(10),
            MakeUnversionedSentinelValue(EValueType::Max),
        },
        .ColumnNonNullValueCounts = {5, 10, 50, 3},
    };
    lhs += rhs;
    EXPECT_EQ(lhs, rhs);
}

TEST(TMergeColumnarStatisticsTest, NonEmptyAndEmpty)
{
    TColumnarStatistics lhs{
        .ColumnDataWeights = {40, 80, 400, 128},
        .ColumnMinValues = {
            MakeUnversionedUint64Value(5),
            MakeUnversionedStringValue("gaga"),
            MakeUnversionedInt64Value(-77777),
            MakeUnversionedSentinelValue(EValueType::Min),
        },
        .ColumnMaxValues = {
            MakeUnversionedUint64Value(500),
            MakeUnversionedStringValue("gugu"),
            MakeUnversionedInt64Value(10),
            MakeUnversionedSentinelValue(EValueType::Max),
        },
        .ColumnNonNullValueCounts = {5, 10, 50, 3},
    };
    TColumnarStatistics rhs = TColumnarStatistics::MakeEmpty(4);
    TColumnarStatistics oldLhs = lhs;
    lhs += rhs;
    EXPECT_EQ(lhs, oldLhs);
}

TEST(TMergeColumnarStatisticsTest, DifferentTypes)
{
    TColumnarStatistics lhs{
        .ColumnDataWeights = {64, 33, 10, 64, 100, 72, 0},
        .ColumnMinValues = {
            MakeUnversionedInt64Value(-10),
            MakeUnversionedStringValue("fax"),
            MakeUnversionedBooleanValue(true),
            MakeUnversionedDoubleValue(0.12),
            MakeUnversionedSentinelValue(EValueType::Min),
            MakeUnversionedUint64Value(42),
            MakeUnversionedNullValue(),
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(20),
            MakeUnversionedStringValue("syrok"),
            MakeUnversionedBooleanValue(true),
            MakeUnversionedDoubleValue(34.43),
            MakeUnversionedSentinelValue(EValueType::Max),
            MakeUnversionedUint64Value(50),
            MakeUnversionedNullValue(),
        },
        .ColumnNonNullValueCounts = {8, 5, 10, 8, 3, 9, 0},
    };

    TColumnarStatistics rhs{
        .ColumnDataWeights = {96, 33, 10, 80, 237, 64, 0},
        .ColumnMinValues = {
            MakeUnversionedInt64Value(-20),
            MakeUnversionedStringValue("b.y"),
            MakeUnversionedBooleanValue(false),
            MakeUnversionedDoubleValue(-100),
            MakeUnversionedSentinelValue(EValueType::Min),
            MakeUnversionedUint64Value(46),
            MakeUnversionedNullValue(),
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(10),
            MakeUnversionedStringValue("pop"),
            MakeUnversionedBooleanValue(false),
            MakeUnversionedDoubleValue(std::numeric_limits<double>::infinity()),
            MakeUnversionedSentinelValue(EValueType::Max),
            MakeUnversionedUint64Value(47),
            MakeUnversionedNullValue(),
        },
        .ColumnNonNullValueCounts = {12, 7, 10, 10, 2, 8, 0},
    };

    lhs += rhs;

    TColumnarStatistics expected{
        .ColumnDataWeights = {160, 66, 20, 144, 337, 136, 0},
        .ColumnMinValues = {
            MakeUnversionedInt64Value(-20),
            MakeUnversionedStringValue("b.y"),
            MakeUnversionedBooleanValue(false),
            MakeUnversionedDoubleValue(-100),
            MakeUnversionedSentinelValue(EValueType::Min),
            MakeUnversionedUint64Value(42),
            MakeUnversionedNullValue(),
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(20),
            MakeUnversionedStringValue("syrok"),
            MakeUnversionedBooleanValue(true),
            MakeUnversionedDoubleValue(std::numeric_limits<double>::infinity()),
            MakeUnversionedSentinelValue(EValueType::Max),
            MakeUnversionedUint64Value(50),
            MakeUnversionedNullValue(),
        },
        .ColumnNonNullValueCounts = {20, 12, 20, 18, 5, 17, 0},
    };
    EXPECT_EQ(lhs, expected);
}

TEST(TMergeColumnarStatisticsTest, DifferentTypesInOneColumn)
{
    TColumnarStatistics lhs{
        .ColumnDataWeights = {100},
        .ColumnMinValues = {MakeUnversionedInt64Value(10)},
        .ColumnMaxValues = {MakeUnversionedBooleanValue(false)},
        .ColumnNonNullValueCounts = {20},
    };

    TColumnarStatistics rhs{
        .ColumnDataWeights = {200},
        .ColumnMinValues = {MakeUnversionedDoubleValue(1.9)},
        .ColumnMaxValues = {MakeUnversionedStringValue("pick")},
        .ColumnNonNullValueCounts = {13},
    };

    lhs += rhs;

    TColumnarStatistics expected{
        .ColumnDataWeights = {300},
        .ColumnMinValues = {MakeUnversionedInt64Value(10)},
        .ColumnMaxValues = {MakeUnversionedStringValue("pick")},
        .ColumnNonNullValueCounts = {33},
    };
    EXPECT_EQ(lhs, expected);
}

TEST(TMergeColumnarStatisticsTest, NoValueStatistics)
{
    TColumnarStatistics lhs{
        .ColumnDataWeights = {64},
        .ColumnMinValues = {MakeUnversionedInt64Value(10)},
        .ColumnMaxValues = {MakeUnversionedInt64Value(22)},
        .ColumnNonNullValueCounts = {8},
    };

    TColumnarStatistics rhs = TColumnarStatistics::MakeEmpty(1, /*hasValueStatistics*/ false);

    lhs += rhs;

    TColumnarStatistics expected{
        .ColumnDataWeights = {64},
        .ColumnMinValues = {},
        .ColumnMaxValues = {},
        .ColumnNonNullValueCounts = {},
    };
    EXPECT_EQ(lhs, expected);
}

TEST(TMergeColumnarStatisticsTest, LegacyStruct)
{
    TColumnarStatistics lhs{
        .ColumnDataWeights = {64},
        .ColumnMinValues = {MakeUnversionedInt64Value(10)},
        .ColumnMaxValues = {MakeUnversionedInt64Value(22)},
        .ColumnNonNullValueCounts = {8},
    };

    TColumnarStatistics rhs = TColumnarStatistics::MakeLegacy(1, 178);

    lhs += rhs;

    TColumnarStatistics expected{
        .ColumnDataWeights = {64},
        .LegacyChunkDataWeight = 178,
        .ColumnMinValues = {},
        .ColumnMaxValues = {},
        .ColumnNonNullValueCounts = {},
    };
    EXPECT_EQ(lhs, expected);
}

TEST(TColumnarStatisticsColumnSelectionTest, ColumnSelect)
{
    TColumnarStatistics statistics{
        .ColumnDataWeights = {64, 33, 10, 64, 100, 72, 0},
        .ColumnMinValues = {
            MakeUnversionedInt64Value(-10),
            MakeUnversionedStringValue("fax"),
            MakeUnversionedBooleanValue(true),
            MakeUnversionedDoubleValue(0.12),
            MakeUnversionedSentinelValue(EValueType::Min),
            MakeUnversionedUint64Value(42),
            MakeUnversionedNullValue(),
        },
        .ColumnMaxValues = {
            MakeUnversionedInt64Value(20),
            MakeUnversionedStringValue("syrok"),
            MakeUnversionedBooleanValue(true),
            MakeUnversionedDoubleValue(34.43),
            MakeUnversionedSentinelValue(EValueType::Max),
            MakeUnversionedUint64Value(50),
            MakeUnversionedNullValue(),
        },
        .ColumnNonNullValueCounts = {8, 5, 10, 8, 3, 9, 0},
    };
    TNameTablePtr nameTable = TNameTable::FromKeyColumns({"buzz", "off", "taken", "sec", "list", "size", "friend"});
    std::vector<TStableName> stableNames = {
        TStableName("friend"),
        TStableName("taken"),
        TStableName("buzz"),
        TStableName("foo"),
        TStableName("list"),
        TStableName("bar"),
    };

    TColumnarStatistics selectedStatistics = statistics.SelectByColumnNames(nameTable, stableNames);

    TColumnarStatistics expected{
        .ColumnDataWeights = {0, 10, 64, 0, 100, 0},
        .ColumnMinValues = {
            MakeUnversionedNullValue(),
            MakeUnversionedBooleanValue(true),
            MakeUnversionedInt64Value(-10),
            MakeUnversionedNullValue(),
            MakeUnversionedSentinelValue(EValueType::Min),
            MakeUnversionedNullValue(),
        },
        .ColumnMaxValues = {
            MakeUnversionedNullValue(),
            MakeUnversionedBooleanValue(true),
            MakeUnversionedInt64Value(20),
            MakeUnversionedNullValue(),
            MakeUnversionedSentinelValue(EValueType::Max),
            MakeUnversionedNullValue(),
        },
        .ColumnNonNullValueCounts = {0, 10, 8, 0, 3, 0},
    };
    EXPECT_EQ(selectedStatistics, expected);
}

} // namespace
} // namespace NYT::NTableClient
