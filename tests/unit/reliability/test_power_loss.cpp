// NFR-010 "no data loss on crash or power loss": fork-and-kill tests for the files the app writes that
// the project store's own test (tests/unit/project/test_crash.cpp: 1,000 bundle saves, [long]) does not cover. A child
// process (this executable re-run, posix_spawn, in the hidden test "reliability.kill-child") writes one kind of file
// in a loop — alternating two versions — and is SIGKILLed at a random moment; afterwards the file is checked as the
// next start of the app finds it (after the app's own start-up recovery).
//
// The write protocol of every file the app writes (audit; our terms: a cache is "recoverable" when a damaged or missing
// copy is detected and rebuilt, user data is "atomic" when a reader finds the old or the new version):
//   project bundles            AtomicWriter bundle protocol, fsync + rename, recovery     atomic   (test_crash.cpp)
//   autosave, resume.json,     AtomicWriter::writeFileAtomically: tmp → fsync → rename   atomic   kind "single"
//     sample sidecars            over the target → fsync folder; recoverLeftovers()
//                                deletes the tmp files at start-up
//   preferences.json           services/settings (its own kill -9 test)                  atomic
//   renders, normalize output  AudioFileWriter: <target>.part → fsync → rename over the   atomic   kind "render"
//                                target → fsync folder (formerly delete + move)
//   takes (Recordings/)        WavWriter: ".<name>.part" → fsync → rename → fsync folder; recoverable kind "take"
//                                a take cut mid-recording is salvaged at start-up
//                                (WavWriter::salvage; formerly an orphaned part)
//   peaks cache                PeakCache::write (JUCE replaceWithData + move); read()     recoverable kind "peaks"
//                                checks magic, sizes, finite values → recomputed
//   glyph atlas cache          tmp → rename; load() checks format, key and a checksum    recoverable kind "atlas"
//   library file operations    FileOps copy/move: tmp → fsync → rename → fsync folder     atomic   (tests/unit/library)
//   SIGKILL keeps the page cache, so these tests prove crash consistency; power-loss consistency rests on the
//   fsync-before-rename and fsync-the-folder steps listed above (the device's Linux fsync flushes the eMMC; FAT
//   volumes use AtomicWriter's journal variant, tests/unit/store).
// Iterations: RELIABILITY_KILLS (default 40 per kind; the [long]-style deep run: RELIABILITY_KILLS=1000).
#include "services/analysis/Analysis.h"
#include "services/render/AudioWriter.h"
#include "services/sampling/WavWriter.h"
#include "services/store/AtomicWriter.h"
#include "ui/render/GlyphAtlas.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace
{
namespace st = services::store;

struct TempDir
{
    juce::File dir;
    explicit TempDir(const char* name)
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile(juce::String("reliability-") + name + "-" + juce::String(::getpid()) + "-" +
                                juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())))
    {
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
};

int iterations()
{
    const char* v = std::getenv("RELIABILITY_KILLS");
    return v != nullptr && std::atoi(v) > 0 ? std::atoi(v) : 40;
}

std::uint32_t rnd(std::uint32_t& x)
{
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return x;
}

// The two versions of each kind (deterministic, distinguishable, different sizes).
std::string singleVersion(int v) { return std::string(v == 0 ? 48 * 1024 : 80 * 1024, v == 0 ? 'a' : 'b') + (v == 0 ? "A" : "B"); }
int renderFrames(int v) { return v == 0 ? 30000 : 47000; }
std::int32_t renderValue(int v) { return v == 0 ? 1111 : -2222; }
services::analysis::PeakOverview peaksVersion(int v)
{
    services::analysis::PeakOverview o;
    o.bucket = 256;
    o.frames = v == 0 ? 256 * 300 : 256 * 517 - 3;
    const auto n = static_cast<std::size_t>((o.frames + o.bucket - 1) / o.bucket);
    for (std::size_t i = 0; i < n; ++i)
    {
        o.min.push_back(-0.001f * static_cast<float>(i % 900) - (v == 0 ? 0.0f : 0.25f));
        o.max.push_back(0.001f * static_cast<float>(i % 900) + (v == 0 ? 0.0f : 0.25f));
    }
    return o;
}
// A take's frame i, channel c (24-bit): exact through WavWriter::toS24.
float takeSample(std::int64_t i, int c) { return static_cast<float>((i * 7 + c * 1001) % 8000 - 4000) / 8388608.0f; }
constexpr int kTakeChunks = 12; // × 65 536 bytes ≈ 0.8 MB per take

// --- the child ---------------------------------------------------------------------------------------------------------
void tick(int fd)
{
    const char c = 'x';
    if (::write(fd, &c, 1) < 0)
        ::_exit(7); // the parent is gone
}

[[noreturn]] void childLoop(const std::string& kind, const juce::File& dir)
{
    const int fd = 3;
    tick(fd); // started
    if (kind == "single")
        for (int v = 1;; v ^= 1)
        {
            const auto text = singleVersion(v);
            (void) st::AtomicWriter::writeFileAtomically(dir.getChildFile("autosave.json"), text.data(), text.size());
            tick(fd);
        }
    if (kind == "render")
        for (int v = 1;; v ^= 1)
        {
            services::render::AudioWriterSpec spec;
            spec.format = services::render::SampleFormat::Pcm16;
            spec.channels = 2;
            auto w = services::render::AudioFileWriter::create(dir.getChildFile("Render.wav"), spec);
            if (w)
            {
                std::vector<std::int32_t> buf(static_cast<std::size_t>(renderFrames(v)) * 2, renderValue(v));
                w.value()->writeCounts(buf.data(), renderFrames(v));
                (void) w.value()->finish();
            }
            tick(fd);
        }
    if (kind == "take")
        for (int n = 1;; ++n)
        {
            services::sampling::WavWriter w;
            const auto target = dir.getChildFile("REC " + juce::String(n).paddedLeft('0', 3) + ".wav");
            if (target.exists())
                continue;
            if (! w.open(target, 44100.0, 2))
                ::_exit(4);
            std::vector<float> l(4096), r(4096);
            const std::int64_t total = kTakeChunks * services::sampling::WavWriter::kChunkBytes / 6;
            for (std::int64_t at = 0; at < total; at += 4096)
            {
                const auto k = static_cast<std::int64_t>(std::min<std::int64_t>(4096, total - at));
                for (std::int64_t i = 0; i < k; ++i)
                {
                    l[static_cast<std::size_t>(i)] = takeSample(at + i, 0);
                    r[static_cast<std::size_t>(i)] = takeSample(at + i, 1);
                }
                (void) w.write(l.data(), r.data(), k);
            }
            (void) w.finish();
            tick(fd);
        }
    if (kind == "peaks")
        for (int v = 1;; v ^= 1)
        {
            (void) services::analysis::PeakCache::write(dir.getChildFile("0123456789abcdef-256.pks"), peaksVersion(v));
            tick(fd);
        }
    if (kind == "atlas")
    {
        ui::GlyphAtlas atlas;
        std::string error;
        if (! atlas.build(0.625f, error))
            ::_exit(5);
        for (;;)
        {
            (void) atlas.save(dir.getChildFile("atlas.bin").getFullPathName().toStdString(), error);
            tick(fd);
        }
    }
    ::_exit(6);
}

// Spawns the child for `kind` writing into `dir`; the read end of its tick pipe in `ticks`.
pid_t spawnChild(const std::string& kind, const juce::File& dir, int& ticks)
{
    int fds[2];
    if (::pipe(fds) != 0)
        return -1;
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString();
    const std::string env[] = {"RELIABILITY_KIND=" + kind, "RELIABILITY_DIR=" + dir.getFullPathName().toStdString()};
    std::vector<char*> envp;
    for (const auto& e : env)
        envp.push_back(const_cast<char*>(e.c_str()));
    for (char** e = environ; *e != nullptr; ++e)
        if (std::strncmp(*e, "RELIABILITY_", 12) != 0)
            envp.push_back(*e);
    envp.push_back(nullptr);
    std::string name = "reliability.kill-child";
    char* argv[] = {const_cast<char*>(exe.c_str()), name.data(), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 3);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int r = ::posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (r != 0)
    {
        ::close(fds[0]);
        return -1;
    }
    ticks = fds[0];
    return pid;
}

struct KillStats
{
    int kills = 0, cycles = 0, midWrite = 0;
};

// Runs `n` children of `kind`, each killed `0 … maxUs` after it started its loop; `verify()` after each kill.
template <class Verify>
KillStats killLoop(const std::string& kind, const juce::File& dir, int n, std::uint32_t maxUs, Verify verify)
{
    KillStats s;
    std::uint32_t x = 0x2545F491u ^ static_cast<std::uint32_t>(kind.size() * 977);
    for (int i = 0; i < n; ++i)
    {
        int ticks = -1;
        const pid_t pid = spawnChild(kind, dir, ticks);
        REQUIRE(pid > 0);
        char c = 0;
        const bool started = ::read(ticks, &c, 1) == 1;
        if (started)
            ::usleep(rnd(x) % (maxUs + 1));
        ::kill(pid, SIGKILL);
        int status = 0;
        REQUIRE(::waitpid(pid, &status, 0) == pid);
        char buf[4096];
        int cycles = 0;
        for (ssize_t got; (got = ::read(ticks, buf, sizeof buf)) > 0;)
            cycles += static_cast<int>(got);
        ::close(ticks);
        INFO(kind << " iteration " << i << ", child status " << status);
        REQUIRE(started);
        REQUIRE((WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL));
        ++s.kills;
        s.cycles += cycles;
        verify(i);
    }
    return s;
}

std::vector<juce::String> names(const juce::File& dir)
{
    std::vector<juce::String> out;
    for (const auto& f : dir.findChildFiles(juce::File::findFilesAndDirectories, false, "*"))
        out.push_back(f.getFileName());
    std::sort(out.begin(), out.end());
    return out;
}

std::uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
} // namespace

TEST_CASE("reliability.kill-child", "[.killchild]")
{
    const char* kind = std::getenv("RELIABILITY_KIND");
    const char* dir = std::getenv("RELIABILITY_DIR");
    if (kind == nullptr || dir == nullptr)
        SKIP("run by the reliability kill tests only");
    childLoop(kind, juce::File(dir));
}

TEST_CASE("kill: SIGKILL during atomic single-file writes (autosave, resume, sidecars) keeps old or new",
          "[reliability]")
{
    TempDir tmp("single");
    const auto target = tmp.dir.getChildFile("autosave.json");
    const auto a = singleVersion(0), b = singleVersion(1);
    REQUIRE(st::AtomicWriter::writeFileAtomically(target, a.data(), a.size()));
    int seenA = 0, seenB = 0;
    const auto s = killLoop("single", tmp.dir, iterations(), 8000, [&](int)
    {
        juce::MemoryBlock m;
        REQUIRE(target.loadFileAsData(m)); // never missing
        const std::string got(static_cast<const char*>(m.getData()), m.getSize());
        REQUIRE((got == a || got == b));   // never torn
        (got == a ? seenA : seenB)++;
        st::AtomicWriter::recoverLeftovers(tmp.dir); // start-up: the interrupted tmp files go
        REQUIRE(names(tmp.dir) == std::vector<juce::String> {"autosave.json"});
    });
    std::printf("reliability single: %d kills, %d completed writes, old %d / new %d\n", s.kills, s.cycles, seenA, seenB);
    CHECK(s.cycles > 0);
}

TEST_CASE("kill: SIGKILL during a render over an existing render keeps a complete file", "[reliability]")
{
    TempDir tmp("render");
    const auto target = tmp.dir.getChildFile("Render.wav");
    {
        services::render::AudioWriterSpec spec;
        spec.format = services::render::SampleFormat::Pcm16;
        auto w = services::render::AudioFileWriter::create(target, spec);
        REQUIRE(w);
        std::vector<std::int32_t> buf(static_cast<std::size_t>(renderFrames(0)) * 2, renderValue(0));
        REQUIRE(w.value()->writeCounts(buf.data(), renderFrames(0)));
        REQUIRE(w.value()->finish());
    }
    const auto s = killLoop("render", tmp.dir, iterations(), 15000, [&](int)
    {
        juce::MemoryBlock m;
        REQUIRE(target.loadFileAsData(m)); // the old render is never deleted before the new one is in place
        const auto* p = static_cast<const unsigned char*>(m.getData());
        REQUIRE(m.getSize() >= 44);
        REQUIRE(std::memcmp(p, "RIFF", 4) == 0);
        REQUIRE(le32(p + 4) == m.getSize() - 8);
        const std::uint32_t data = le32(p + 40);
        REQUIRE(data == m.getSize() - 44);
        const int v = data == static_cast<std::uint32_t>(renderFrames(0)) * 4 ? 0 : 1;
        REQUIRE(data == static_cast<std::uint32_t>(renderFrames(v)) * 4);
        for (std::size_t i = 44; i < m.getSize(); i += 2)
            REQUIRE(static_cast<std::int16_t>(p[i] | p[i + 1] << 8) == renderValue(v));
        for (const auto& n : names(tmp.dir)) // only the render and, mid-write, its part file (never listed: .part)
            REQUIRE((n == "Render.wav" || n == "Render.wav.part"));
    });
    std::printf("reliability render: %d kills, %d completed renders\n", s.kills, s.cycles);
}

TEST_CASE("kill: SIGKILL during recordings leaves complete takes, the cut one is salvaged", "[reliability]")
{
    TempDir tmp("take");
    int salvaged = 0, removed = 0;
    const auto s = killLoop("take", tmp.dir, iterations(), 40000, [&](int)
    {
        const auto lines = services::sampling::WavWriter::salvage(tmp.dir); // what the next start does
        for (const auto& line : lines)
        {
            INFO(line);
            REQUIRE(line.find("FAILED") == std::string::npos);
            (line.find("removed") != std::string::npos ? removed : salvaged)++;
        }
        REQUIRE(lines.size() <= 1); // one take was in flight at most
        for (const auto& f : tmp.dir.findChildFiles(juce::File::findFilesAndDirectories, false, "*"))
        {
            INFO(f.getFileName());
            REQUIRE(f.getFileName().endsWith(".wav")); // no part file stays behind
            juce::MemoryBlock m;
            REQUIRE(f.loadFileAsData(m));
            const auto* p = static_cast<const unsigned char*>(m.getData());
            REQUIRE(m.getSize() > 44);
            REQUIRE(le32(p + 4) == m.getSize() - 8);
            REQUIRE(le32(p + 40) == m.getSize() - 44);
            REQUIRE((le32(p + 40) % 6) == 0);
            const std::int64_t frames = le32(p + 40) / 6;
            for (std::int64_t i = 0; i < frames; i += 97) // every 97th frame: the written signal, in order
                for (int c = 0; c < 2; ++c)
                {
                    const unsigned char* q = p + 44 + i * 6 + c * 3;
                    const std::int32_t v = (static_cast<std::int32_t>(q[0] | q[1] << 8 | q[2] << 16) << 8) >> 8;
                    REQUIRE(v == services::sampling::WavWriter::toS24(takeSample(i, c)));
                }
            f.deleteFile();
        }
    });
    std::printf("reliability take: %d kills, %d complete takes, %d salvaged, %d empty parts removed\n", s.kills,
                s.cycles, salvaged, removed);
    CHECK(salvaged > 0); // the kills do land mid-recording
}

TEST_CASE("kill: a take part file without audio or with a foreign header", "[reliability]")
{
    TempDir tmp("take-edge");
    tmp.dir.getChildFile(".REC 001.wav.part").replaceWithText("");          // cut before the header
    tmp.dir.getChildFile(".REC 002.wav.part").replaceWithText("RIFF....WAVE"); // cut inside the header
    juce::MemoryOutputStream foreign;
    foreign.write("RIFF\0\0\0\0WAVEfmt ", 16);
    for (int i = 0; i < 100; ++i)
        foreign.writeInt(i);
    tmp.dir.getChildFile(".mine.wav.part").replaceWithData(foreign.getData(), foreign.getDataSize());
    {
        // this writer's header and less than one frame of audio (a cut right after open())
        unsigned char h[44 + 2] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                                   1, 0, 1, 0, 0x80, 0xBB, 0, 0, 0x80, 0x32, 0x02, 0, 3, 0, 24, 0,
                                   'd', 'a', 't', 'a', 0, 0, 0, 0, 7, 7};
        tmp.dir.getChildFile(".REC 003.wav.part").replaceWithData(h, sizeof h);
    }
    tmp.dir.getChildFile("REC 004.wav").replaceWithText("taken"); // the target name exists
    {
        services::sampling::WavWriter w;
        REQUIRE(w.open(tmp.dir.getChildFile("REC 005.wav"), 44100.0, 2));
        std::vector<float> l(30000, 0.25f);
        REQUIRE(w.write(l.data(), nullptr, static_cast<std::int64_t>(l.size()))); // > one 64 KB chunk on disk
        juce::File keep = w.partFile().getSiblingFile("copy4.tmp");
        REQUIRE(w.partFile().copyFileTo(keep));
        w.abandon();
        keep.moveFileTo(tmp.dir.getChildFile(".REC 004.wav.part"));
    }
    const auto lines = services::sampling::WavWriter::salvage(tmp.dir);
    std::string all;
    for (const auto& l : lines)
        all += l + "\n";
    INFO(all);
    CHECK(all.find(".REC 001.wav.part removed (no header)") != std::string::npos);
    CHECK(all.find(".REC 002.wav.part removed (no header)") != std::string::npos);
    CHECK(all.find(".REC 003.wav.part removed (no audio)") != std::string::npos);
    CHECK(all.find(".REC 004.wav.part -> REC 004 recovered.wav frames=") != std::string::npos);
    CHECK(tmp.dir.getChildFile(".mine.wav.part").existsAsFile()); // not this writer's: left alone
    CHECK(tmp.dir.getChildFile("REC 004.wav").loadFileAsString() == "taken");
    const auto rec = tmp.dir.getChildFile("REC 004 recovered.wav");
    REQUIRE(rec.existsAsFile());
    CHECK((rec.getSize() - 44) % 6 == 0);
    CHECK(rec.getSize() > 44 + 6 * 10000);
}

TEST_CASE("kill: SIGKILL while the peaks cache is written: read() gives a version or nothing", "[reliability]")
{
    TempDir tmp("peaks");
    const auto file = tmp.dir.getChildFile("0123456789abcdef-256.pks");
    const auto a = peaksVersion(0), b = peaksVersion(1);
    int hits = 0, misses = 0;
    const auto s = killLoop("peaks", tmp.dir, iterations(), 5000, [&](int)
    {
        const auto r = services::analysis::PeakCache::read(file);
        if (r)
        {
            REQUIRE((*r == a || *r == b));
            ++hits;
        }
        else
            ++misses; // a missing / damaged copy: PeakCache::get recomputes and rewrites it
    });
    std::printf("reliability peaks: %d kills, %d writes, read %d / rebuilt %d\n", s.kills, s.cycles, hits, misses);
}

TEST_CASE("kill: SIGKILL while the glyph atlas cache is written: load() is exact or refuses", "[reliability]")
{
    TempDir tmp("atlas");
    const auto path = tmp.dir.getChildFile("atlas.bin").getFullPathName().toStdString();
    int loads = 0, refused = 0;
    ui::GlyphAtlas reference;
    std::string error;
    REQUIRE(reference.build(0.625f, error));
    const auto s = killLoop("atlas", tmp.dir, std::max(4, iterations() / 4), 60000, [&](int)
    {
        ui::GlyphAtlas loaded;
        std::string why;
        if (loaded.load(path, 0.625f, why))
        {
            REQUIRE(loaded.width() == reference.width());
            REQUIRE(loaded.height() == reference.height());
            REQUIRE(std::equal(reference.pixels(), reference.pixels() + reference.width() * reference.height(),
                               loaded.pixels()));
            ++loads;
        }
        else
            ++refused; // missing or torn: the next start builds and rewrites it
    });
    std::printf("reliability atlas: %d kills, %d saves, loaded %d / rebuilt %d\n", s.kills, s.cycles, loads, refused);
}
