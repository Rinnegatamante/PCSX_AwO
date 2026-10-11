#include <assert.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "../frontend/vita/disc_grouping.h"

using vita_disc_grouping::Candidate;

static bool group(const std::vector<Candidate> &input, std::vector<unsigned> *order,
                  std::string *filename, std::string *error)
{
    return vita_disc_grouping::validate(input, order, filename, error);
}

int main()
{
    std::vector<unsigned> order;
    std::string name, error;
    std::vector<Candidate> metal_gear = {
        { "PSX Metal gear Solid ITA CD2 avercab0001.img",
          "PSX Metal gear Solid ITA CD2 avercab0001", 0, "" },
        { "PSX Metal gear Solid ITA CD1 avercab0001.img",
          "PSX Metal gear Solid ITA CD1 avercab0001", 0, "" }
    };
    assert(group(metal_gear, &order, &name, &error));
    assert(order.size() == 2 && order[0] == 1 && order[1] == 0);
    assert(name == "PSX Metal gear Solid ITA avercab0001.m3u");

    const std::vector<Candidate> redump = {
        { "Final Fantasy VII (USA) (Disc 1).cue", "Final Fantasy VII (USA) (Disc 1)", 77, "SLUS00645" },
        { "Final Fantasy VII (USA) (Disc 2).cue", "Final Fantasy VII (USA) (Disc 2)", 77, "SLUS00646" },
        { "Final Fantasy VII (USA) (Disc 3).cue", "Final Fantasy VII (USA) (Disc 3)", 77, "SLUS00647" }
    };
    assert(group(redump, &order, &name, &error));
    assert(name == "Final Fantasy VII (USA).m3u");
    assert(order[0] == 0 && order[1] == 1 && order[2] == 2);

    std::vector<Candidate> different = redump;
    different[1].game_hash = 85;
    assert(!group(different, &order, &name, &error));
    assert(error == "Selected discs have different game IDs.");

    different = redump;
    different[1].title = "Resident Evil (USA) (Disc 2)";
    different[1].game_hash = 0;
    assert(!group(different, &order, &name, &error));

    different = redump;
    different[1].title = "Final Fantasy VII (USA) (Disc 1)";
    assert(!group(different, &order, &name, &error));
    assert(error == "Duplicate disc number.");

    different = redump;
    different.erase(different.begin());
    assert(!group(different, &order, &name, &error));
    assert(error == "Select Disc 1 as well.");

    const std::vector<Candidate> unnamed = {
        { "game-a.chd", "game a", 1234, "SERIAL1" },
        { "game-b.chd", "game b", 1234, "SERIAL2" }
    };
    assert(group(unnamed, &order, &name, &error));
    assert(order[0] == 0 && order[1] == 1);

    different = unnamed;
    different[1].game_hash = 0;
    assert(!group(different, &order, &name, &error));

    different = unnamed;
    different[1].serial = "SERIAL1";
    assert(!group(different, &order, &name, &error));
    assert(error == "Cannot distinguish the selected discs.");

    different = redump;
    different[1].filename = "../escaped.cue";
    assert(!group(different, &order, &name, &error));
    assert(error == "Invalid disc filename.");
    return 0;
}
