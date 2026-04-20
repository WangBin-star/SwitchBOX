#pragma once

#include <vector>

#include <borealis.hpp>

#include "switchbox/core/playback_history.hpp"

namespace switchbox::app {

class HistoryActivity : public brls::Activity {
public:
    HistoryActivity();
    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    void reload_entries();
    void open_history_entry(size_t index);

    brls::Box* content_box = nullptr;
    std::vector<switchbox::core::PlaybackHistoryEntry> entries;
};

}  // namespace switchbox::app
