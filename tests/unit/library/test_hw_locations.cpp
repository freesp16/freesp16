// The device's library locations — the internal disk's stock Samples tree as the read-only "$Ext:INTERNAL"
// location, every FAT stick read-only, the data directory's own (bind) mount skipped — and the file operations that
// must never write into a read-only location. Also InternalDisk::projects (what `cmd import-sp16 internal:` lists).
#include "LibTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using services::library::InternalDisk;
using services::library::ProcMountsProvider;
using services::library::SortOrder;

TEST_CASE("lib.hw: /proc/mounts on the device — internal disk, sticks, the data dir's bind mount", "[library][hw]")
{
    libtest::TempDir tmp("hwmounts");
    const juce::File emmc = tmp / "emmc";
    emmc.getChildFile(InternalDisk::kStockFolder).getChildFile("Samples").getChildFile("Drums").createDirectory();
    const std::string text = "/dev/root / ext4 rw 0 0\n"
                             "/dev/mmcblk0p1 " + emmc.getFullPathName().toStdString() + " vfat rw,noatime 0 0\n"
                             "/dev/sda1 /media/usb0 vfat rw 0 0\n"
                             "/dev/sda1 /root/data vfat rw 0 0\n" // the launcher's bind mount of the stick's data
                             "ubi0:data /root/data2 ubifs rw 0 0\n";
    const auto parsed = ProcMountsProvider::parse(text, emmc.getFullPathName().toStdString(), {"/root/data/app"});
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].alias == InternalDisk::kAlias);
    CHECK(parsed[0].root == emmc.getChildFile(InternalDisk::kStockFolder).getChildFile("Samples"));
    CHECK(parsed[0].readOnly);
    CHECK(parsed[0].removable);
    CHECK(parsed[1].alias == "$Ext:usb0");
    CHECK(parsed[1].readOnly);

    // Without the stock folder the whole mount is the root; the UI name is the alias's tail, never the folder name.
    libtest::TempDir bare("hwbare");
    const auto plain = ProcMountsProvider::parse("/dev/mmcblk0p1 " + bare.dir().getFullPathName().toStdString()
                                                     + " vfat rw 0 0\n",
                                                 bare.dir().getFullPathName().toStdString());
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].root == bare.dir());
    CHECK(juce::String(InternalDisk::kAlias).fromFirstOccurrenceOf(":", false, false) == "INTERNAL");
}

TEST_CASE("lib.hw: nothing is written into a read-only location", "[library][hw][fileops]")
{
    libtest::Fixture f;
    const juce::File mount = f.tmp / "emmc";
    const juce::File stockSamples = mount.getChildFile(InternalDisk::kStockFolder).getChildFile("Samples");
    stockSamples.getChildFile("Drums").createDirectory();
    const juce::File unitKick = libtest::writeSample(stockSamples.getChildFile("Drums/kick.wav"), 100);
    const juce::File mounts = f.tmp / "mounts";
    REQUIRE(mounts.replaceWithText("/dev/mmcblk0p1 " + mount.getFullPathName() + " vfat rw 0 0\n"));
    f.library.setLocationProvider(
        std::make_shared<ProcMountsProvider>(mounts, mount.getFullPathName().toStdString()));
    f.library.pollLocations();
    f.workers.drain();
    REQUIRE(f.library.findLocation(InternalDisk::kAlias) != nullptr);
    CHECK(f.library.isReadOnly(unitKick));
    CHECK(f.library.isReadOnly(stockSamples));
    CHECK_FALSE(f.library.isReadOnly(f.samples()));
    // Listed through its alias like any location.
    const auto listed = f.library.listNow(juce::String(InternalDisk::kAlias) + "/Drums", SortOrder::AToZ);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].aliasPath == juce::String(InternalDisk::kAlias) + "/Drums/kick.wav");

    auto wait = [&](auto start)
    {
        std::optional<core::Result<juce::File>> out;
        start([&](core::Result<juce::File> r) { out.emplace(std::move(r)); });
        f.workers.drain();
        REQUIRE(out.has_value());
        return std::move(*out);
    };
    const juce::File own = libtest::writeSample(f.samples().getChildFile("own.wav"), 100);
    auto refusedCopy = wait([&](auto done) { f.library.copy(own, stockSamples, done); });
    CHECK_FALSE(refusedCopy.ok());
    CHECK(refusedCopy.error().message.find("read-only") != std::string::npos);
    CHECK_FALSE(stockSamples.getChildFile("own.wav").exists());
    CHECK_FALSE(wait([&](auto done) { f.library.move(unitKick, f.samples(), true, done); }).ok());
    CHECK_FALSE(wait([&](auto done) { f.library.move(own, stockSamples, true, done); }).ok());
    CHECK_FALSE(wait([&](auto done) { f.library.rename(unitKick, "other.wav", true, done); }).ok());
    CHECK_FALSE(wait([&](auto done) { f.library.trash(unitKick, true, done); }).ok());
    CHECK_FALSE(wait([&](auto done) { f.library.newFolder(stockSamples, "New", done); }).ok());
    CHECK(unitKick.existsAsFile());
    CHECK_FALSE(stockSamples.getChildFile("New").exists());
    std::optional<std::vector<juce::File>> imported;
    f.library.importFiles(juce::StringArray(own.getFullPathName()), stockSamples,
                          [&](std::vector<juce::File> files) { imported = std::move(files); });
    f.workers.drain();
    REQUIRE(imported.has_value());
    CHECK(imported->empty());
    // Copying out of it into the library is fine.
    auto copied = wait([&](auto done) { f.library.copy(unitKick, f.samples(), done); });
    REQUIRE(copied.ok());
    CHECK(copied.value().getParentDirectory() == f.samples());
    CHECK(unitKick.existsAsFile());
}

TEST_CASE("lib.hw: the internal disk's projects (import-sp16 internal:)", "[library][hw]")
{
    libtest::TempDir tmp("hwprj");
    const juce::File projects = InternalDisk::projectsFolder(tmp.dir());
    CHECK(projects == tmp / "Projects"); // no stock folder: the mount itself
    CHECK(InternalDisk::projects(tmp.dir()).empty());
    const juce::File stock = tmp.dir().getChildFile(InternalDisk::kStockFolder);
    stock.getChildFile("Projects").createDirectory();
    for (const char* name : {"Beat 10.prj", "Beat 2.prj", ".hidden.prj", "notes.txt", "Alpha.PRJ"})
        REQUIRE(stock.getChildFile("Projects").getChildFile(name).replaceWithText("x"));
    CHECK(InternalDisk::projectsFolder(tmp.dir()) == stock.getChildFile("Projects"));
    const auto found = InternalDisk::projects(tmp.dir());
    REQUIRE(found.size() == 3);
    CHECK(found[0].getFileName() == "Alpha.PRJ");
    CHECK(found[1].getFileName() == "Beat 2.prj");
    CHECK(found[2].getFileName() == "Beat 10.prj");
}
