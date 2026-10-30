// PC Extras Soul Tree viewer, session side (ps2_soul_session.h) and the read-only policy of throw-away sessions (ps2_session_policy.h).
#include "MiniTest.h"
#include "ps2_session_policy.h"
#include "ps2_soul_session.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_rom_device.h"
#include "runtime/ps2_vfs.h"

#include <filesystem>
#include <fstream>
#include <set>

void register_ps2_soul_session_tests()
{
    using namespace pc_extras::soul_session;
    MiniTest::Case("SoulSession", [](TestCase &tc)
    {
        tc.Run("the table has the 13 characters and exactly the 55 events of the game", [](TestCase &t)
        {
            size_t count = 0;
            const Character *chars = characters(count);
            t.Equals(static_cast<int>(count), 13, "13 characters");
            int total = 0;
            std::set<uint32_t> labels, variables;
            for (size_t i = 0; i < count; ++i)
            {
                total += chars[i].doors;
                variables.insert(chars[i].doorVariable);
                for (int d = 0; d < chars[i].doors; ++d)
                    labels.insert(chars[i].firstLabel + 2u * static_cast<uint32_t>(d));
            }
            t.Equals(total, 55, "55 doors");
            t.Equals(static_cast<int>(labels.size()), 55, "no two doors share a label");
            t.Equals(static_cast<int>(variables.size()), 13, "one door variable per character");
            t.IsTrue(*labels.begin() == 2000u && *labels.rbegin() == 2108u, "labels span 2000..2108");
        });

        tc.Run("--soul-event requests resolve to the original label and door variable, and only for doors that exist", [](TestCase &t)
        {
            Request r;
            t.IsTrue(parseRequest("seena:5", r), "seena:5");
            t.Equals(r.label, 2078u, "label"); t.Equals(r.variable, 64u, "variable"); t.Equals(r.value, 4u, "door - 1");
            t.IsTrue(parseRequest("trihard:2", r), "trihard has two doors");
            t.Equals(r.label, 2102u, "trihard door 2");
            t.IsTrue(parseRequest("souma:1", r), "souma has one door");
            t.Equals(r.label, 2104u, "souma door 1");
            t.IsFalse(parseRequest("souma:2", r), "souma has no second door");
            t.IsFalse(parseRequest("hilda:3", r), "hilda has two doors");
            t.IsFalse(parseRequest("seena:0", r), "doors start at 1");
            t.IsFalse(parseRequest("seena:6", r), "no sixth door");
            t.IsFalse(parseRequest("nobody:1", r), "unknown character");
            t.IsFalse(parseRequest("seena", r), "missing door");
            t.IsFalse(parseRequest("seena:2x", r), "garbage after the number");
        });

        tc.Run("door titles are the English 'Door of the Heart' ordinals", [](TestCase &t)
        {
            t.Equals(doorTitle(1), std::string("First Door of the Heart"), "1");
            t.Equals(doorTitle(5), std::string("Fifth Door of the Heart"), "5");
            t.IsTrue(doorTitle(0).empty() && doorTitle(6).empty(), "outside 1..5");
        });

        tc.Run("read-only session: the guest file system refuses every write and still reads", [](TestCase &t)
        {
            const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ps2x_soul_policy_test";
            std::filesystem::create_directories(dir);
            { std::ofstream f(dir / "keep.bin", std::ios::binary); f << "x"; }
            PS2VfsMounts mounts;
            mounts.hostRoot = dir; mounts.cdRoot = dir; mounts.memoryCard0Root = dir;
            PS2RomDevice rom;
            PS2Vfs vfs;
            const int32_t writable = vfs.open("mc0:/new.bin", PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT, mounts, rom);
            t.IsTrue(writable >= 0, "writing works in a normal run");
            if (writable >= 0) (void)vfs.close(writable);
            std::filesystem::remove(dir / "new.bin");

            ps2x::session::setReadOnly();
            t.IsTrue(vfs.open("mc0:/new2.bin", PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT, mounts, rom) < 0, "create refused");
            t.IsTrue(vfs.open("mc0:/keep.bin", PS2_FIO_O_RDWR, mounts, rom) < 0, "read/write refused");
            t.IsTrue(vfs.open("mc0:/keep.bin", PS2_FIO_O_WRONLY | PS2_FIO_O_TRUNC, mounts, rom) < 0, "truncate refused");
            const int32_t reader = vfs.open("mc0:/keep.bin", PS2_FIO_O_RDONLY, mounts, rom);
            t.IsTrue(reader >= 0, "reading still works");
            if (reader >= 0) (void)vfs.close(reader);
            t.IsFalse(std::filesystem::exists(dir / "new2.bin"), "nothing was created");
            ps2x::session::g_readOnly.store(false); // the policy is process-wide: restore it for the other tests
            std::filesystem::remove_all(dir);
        });
    });
}
