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
    void install_common_actions();
    void update_common_action_availability();
    void reload_entries(const std::string& preferred_focus_stable_key = {});
    void open_history_entry(size_t index);
    void confirm_delete_history_entry(size_t index);
    void confirm_clear_history();
    void focus_history_entry_by_stable_key(const std::string& preferred_focus_stable_key);

    brls::Box* content_box = nullptr;
    brls::ActionIdentifier action_clear_id = ACTION_NONE;
    std::vector<switchbox::core::PlaybackHistoryEntry> entries;
};

}  // namespace switchbox::app
