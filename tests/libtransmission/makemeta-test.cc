// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstdlib> // mktemp()
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "transmission.h"

#include "crypto-utils.h"
#include "file.h"
#include "makemeta.h"
#include "session.h" // TR_NAME
#include "torrent-metainfo.h"

#include "test-fixtures.h"

using namespace std::literals;

namespace libtransmission::test
{

class MakemetaTest : public SandboxedTest
{
protected:
    static auto constexpr DefaultMaxFileCount = size_t{ 16 };
    static auto constexpr DefaultMaxFileSize = size_t{ 1024 };

    auto makeRandomFiles(
        std::string_view top,
        size_t n_files = std::max(size_t{ 1U }, static_cast<size_t>(tr_rand_int_weak(DefaultMaxFileCount))),
        size_t max_size = DefaultMaxFileSize)
    {
        auto files = std::vector<std::pair<std::string, std::vector<std::byte>>>{};

        for (size_t i = 0; i < n_files; ++i)
        {
            auto payload = std::vector<std::byte>{};
            // TODO(5.0.0): zero-sized files are disabled in these test
            // because tr_torrent_metainfo discards them, throwing off the
            // builder-to-metainfo comparisons here. tr_torrent_metainfo
            // will behave when BEP52 support is added in Transmission 5.
            static auto constexpr MinFileSize = size_t{ 1U };
            payload.resize(std::max(MinFileSize, static_cast<size_t>(tr_rand_int_weak(max_size))));
            tr_rand_buffer(std::data(payload), std::size(payload));

            auto filename = tr_pathbuf{ top, '/', "test.XXXXXX" };
            createTmpfileWithContents(std::data(filename), std::data(payload), std::size(payload));
            tr_sys_path_native_separators(std::data(filename));

            files.emplace_back(std::string{ filename.sv() }, payload);
        }

        return files;
    }

    static auto testBuilder(tr_metainfo_builder& builder)
    {
        tr_error* error = builder.makeChecksums().get();
        EXPECT_EQ(error, nullptr) << *error;

        auto metainfo = tr_torrent_metainfo{};
        EXPECT_TRUE(metainfo.parseBenc(builder.benc()));
        EXPECT_EQ(builder.fileCount(), metainfo.fileCount());
        EXPECT_EQ(builder.pieceSize(), metainfo.pieceSize());
        EXPECT_EQ(builder.totalSize(), metainfo.totalSize());
        EXPECT_EQ(builder.totalSize(), metainfo.totalSize());
        for (size_t i = 0, n = std::min(builder.fileCount(), metainfo.fileCount()); i < n; ++i)
        {
            EXPECT_EQ(builder.fileSize(i), metainfo.files().fileSize(i));
            EXPECT_EQ(builder.path(i), metainfo.files().path(i));
        }
        EXPECT_EQ(builder.name(), metainfo.name());
        EXPECT_EQ(builder.comment(), metainfo.comment());
        EXPECT_EQ(builder.isPrivate(), metainfo.isPrivate());
        EXPECT_EQ(builder.announceList().toString(), metainfo.announceList().toString());
        return metainfo;
    }
};

TEST_F(MakemetaTest, comment)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();
    auto builder = tr_metainfo_builder{ filename };

    static auto constexpr Comment = "This is the comment"sv;
    builder.setComment(Comment);

    EXPECT_EQ(Comment, testBuilder(builder).comment());
}

TEST_F(MakemetaTest, source)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();
    auto builder = tr_metainfo_builder{ filename };

    static auto constexpr Source = "This is the source"sv;
    builder.setSource(Source);

    EXPECT_EQ(Source, testBuilder(builder).source());
}

TEST_F(MakemetaTest, isPrivate)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();

    for (bool const is_private : { true, false })
    {
        auto builder = tr_metainfo_builder{ filename };
        builder.setPrivate(is_private);
        EXPECT_EQ(is_private, testBuilder(builder).isPrivate());
    }
}

TEST_F(MakemetaTest, pieceSize)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();

    for (uint32_t const piece_size : { 16384, 32768 })
    {
        auto builder = tr_metainfo_builder{ filename };
        builder.setPieceSize(piece_size);
        EXPECT_EQ(piece_size, testBuilder(builder).pieceSize());
    }
}

TEST_F(MakemetaTest, webseeds)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();
    auto builder = tr_metainfo_builder{ filename };

    static auto constexpr Webseed = "https://www.example.com/linux.iso"sv;
    builder.setWebseeds(std::vector<std::string>{ std::string{ Webseed } });

    auto const metainfo = testBuilder(builder);
    EXPECT_EQ(1U, metainfo.webseedCount());
    EXPECT_EQ(Webseed, metainfo.webseed(0));
}

TEST_F(MakemetaTest, nameIsRootSingleFile)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();
    auto builder = tr_metainfo_builder{ filename };
    EXPECT_EQ(tr_sys_path_basename(filename), testBuilder(builder).name());
}

TEST_F(MakemetaTest, anonymizeTrue)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();

    auto builder = tr_metainfo_builder{ filename };
    builder.setAnonymize(true);
    auto const metainfo = testBuilder(builder);
    EXPECT_EQ(""sv, metainfo.creator());
    EXPECT_EQ(time_t{}, metainfo.dateCreated());
}

TEST_F(MakemetaTest, anonymizeFalse)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();

    auto builder = tr_metainfo_builder{ filename };
    builder.setAnonymize(false);
    auto const metainfo = testBuilder(builder);
    EXPECT_TRUE(tr_strvContains(metainfo.creator(), TR_NAME)) << metainfo.creator();
    auto const now = time(nullptr);
    EXPECT_LE(metainfo.dateCreated(), now);
    EXPECT_LE(now - 60, metainfo.dateCreated());
}

TEST_F(MakemetaTest, nameIsRootMultifile)
{
    auto const files = makeRandomFiles(sandboxDir(), 10);
    auto const [filename, payload] = files.front();
    auto builder = tr_metainfo_builder{ filename };
    EXPECT_EQ(tr_sys_path_basename(filename), testBuilder(builder).name());
}

TEST_F(MakemetaTest, singleFile)
{
    auto const files = makeRandomFiles(sandboxDir(), 1);
    auto const [filename, payload] = files.front();
    auto builder = tr_metainfo_builder{ filename };

    auto trackers = tr_announce_list{};
    trackers.add("udp://tracker.openbittorrent.com:80"sv, trackers.nextTier());
    trackers.add("udp://tracker.publicbt.com:80"sv, trackers.nextTier());
    builder.setAnnounceList(std::move(trackers));

    static auto constexpr Comment = "This is the comment"sv;
    builder.setComment(Comment);

    static auto constexpr IsPrivate = false;
    builder.setPrivate(IsPrivate);

    static auto constexpr Anonymize = false;
    builder.setAnonymize(Anonymize);

    testBuilder(builder);
}

} // namespace libtransmission::test
