#pragma once

#include <string>
#include <vector>

#include <borealis.hpp>

namespace switchbox::app {

struct PlaceholderSection {
    std::string title;
    std::string subtitle;
    std::vector<std::string> checkpoints;
};

class PlaceholderActivity : public brls::Activity {
public:
    explicit PlaceholderActivity(PlaceholderSection section);

private:
    PlaceholderSection section;
};

}  // namespace switchbox::app
