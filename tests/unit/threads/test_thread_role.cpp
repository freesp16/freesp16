// Thread roles (core/util/ThreadRole.h): the map syntax, the defaults, and apply() on a real thread (nice, cores, name).
#include "core/util/ThreadRole.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>

#if defined(__linux__)
 #include <pthread.h>
 #include <sched.h>
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

using namespace core::threads;

namespace
{
struct ResetRoles // the table is process-wide: every case starts and ends on the defaults
{
    ResetRoles() { reset(); }
    ~ResetRoles() { reset(); }
};
} // namespace

TEST_CASE("thread roles: defaults put audio and MIDI above the UI and the workers below it", "[threads]")
{
    ResetRoles roles;
    CHECK(spec(Role::Audio).realtime);
    CHECK(spec(Role::Audio).priority == 70);
    CHECK(spec(Role::Clock).priority == 80);
    CHECK(spec(Role::MidiIn).priority == 75);
    CHECK(spec(Role::Panel).priority == 72);
    CHECK(spec(Role::MidiOut).priority == 65);
    CHECK(spec(Role::Strip).priority == 60);
    CHECK(spec(Role::Recorder).priority == 40);
    CHECK_FALSE(spec(Role::Ui).realtime);
    CHECK(spec(Role::Ui).priority == 0);
    CHECK_FALSE(spec(Role::Worker).realtime);
    CHECK(spec(Role::Worker).priority == 10);
    for (int i = 0; i < static_cast<int>(Role::Count); ++i)
        CHECK(spec(static_cast<Role>(i)).cores == 0); // no map: every role keeps its inherited cores
    CHECK(std::string(roleName(Role::MidiOut)) == "midi_out");
}

TEST_CASE("thread roles: the map sets cores and priorities, bad entries are named and skipped", "[threads]")
{
    ResetRoles roles;
    std::string note;
    CHECK(configure("audio:1@75, worker:0+3@15 ,ui:*,strip:-", note));
    CHECK(note.empty());
    CHECK(spec(Role::Audio).cores == 0b10u);
    CHECK(spec(Role::Audio).priority == 75);
    CHECK(spec(Role::Worker).cores == 0b1001u);
    CHECK(spec(Role::Worker).priority == 15);
    CHECK(spec(Role::Ui).cores == kAllCores);
    CHECK(spec(Role::Strip).cores == 0);
    CHECK(spec(Role::Strip).priority == 60); // no "@": the priority stays
    CHECK(describe().find("audio:1@75") != std::string::npos);
    CHECK(describe().find("worker:0+3@nice15") != std::string::npos);
    CHECK(describe().find("ui:*@nice0") != std::string::npos);

    // A second map goes over the first (the setting over the platform's).
    CHECK(configure("audio:2", note));
    CHECK(spec(Role::Audio).cores == 0b100u);
    CHECK(spec(Role::Audio).priority == 75);

    note.clear();
    CHECK_FALSE(configure("bogus:1,audio:x,midi_in:2@0,worker:1@30,panel,clock:3@99", note));
    CHECK(note == "bogus:1, audio:x, midi_in:2@0, worker:1@30, panel");
    CHECK(spec(Role::Audio).cores == 0b100u); // unchanged by its bad entry
    CHECK(spec(Role::MidiIn).cores == 0);
    CHECK(spec(Role::Clock).cores == 0b1000u); // the good entry after the bad ones still applies
    CHECK(spec(Role::Clock).priority == 99);

    reset();
    CHECK(spec(Role::Audio).cores == 0);
    CHECK(spec(Role::Audio).priority == 70);
}

#if defined(__linux__)
TEST_CASE("thread roles: apply() names, pins and nices the calling thread", "[threads]")
{
    ResetRoles roles;
    std::string note;
    REQUIRE(configure("worker:0@12", note));
    Applied applied;
    int nice = -100;
    cpu_set_t cores;
    CPU_ZERO(&cores);
    char name[16] = {};
    std::thread t(
        [&]
        {
            applied = apply(Role::Worker, "worker-under-test"); // truncated to 15 characters
            errno = 0;
            nice = ::getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)));
            ::sched_getaffinity(0, sizeof cores, &cores);
            pthread_getname_np(pthread_self(), name, sizeof name);
        });
    t.join();
    CHECK(applied.niced);
    CHECK(applied.pinned);
    CHECK_FALSE(applied.realtime);
    CHECK(nice == 12);
    CHECK(CPU_COUNT(&cores) == 1);
    CHECK(CPU_ISSET(0, &cores));
    CHECK(std::string(name) == "worker-under-te");
}

TEST_CASE("thread roles: a real-time role is best effort and leaves unmapped cores alone", "[threads]")
{
    ResetRoles roles;
    cpu_set_t before, after;
    CPU_ZERO(&before);
    CPU_ZERO(&after);
    Applied applied;
    int policy = -1;
    std::thread t(
        [&]
        {
            ::sched_getaffinity(0, sizeof before, &before);
            applied = apply(Role::Recorder, "rec-under-test");
            sched_param p {};
            pthread_getschedparam(pthread_self(), &policy, &p);
            ::sched_getaffinity(0, sizeof after, &after);
        });
    t.join();
    CHECK_FALSE(applied.pinned);
    CHECK(CPU_EQUAL(&before, &after));
    CHECK(policy == (applied.realtime ? SCHED_FIFO : SCHED_OTHER)); // granted (root, rtprio) or refused (a desktop)
}
#endif
