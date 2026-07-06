#include <pystring.h>
#include <borealis/core/i18n.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/views/cells/cell_input.hpp>
#include <vector>

#include "activity/player_activity.hpp"
#include "fragment/setting_sponsor_block.hpp"
#include "utils/config_helper.hpp"
#include "view/selector_cell.hpp"

using namespace brls::literals;

namespace {

constexpr const char* SPONSOR_BLOCK_DEFAULT_SERVER = "https://bsbsb.top";

std::vector<std::string> sponsorBlockStrategyLabels() {
    return {
        "wiliwili/setting/app/playback/sponsor_block_strategy/off"_i18n,
        "wiliwili/setting/app/playback/sponsor_block_strategy/mark"_i18n,
        "wiliwili/setting/app/playback/sponsor_block_strategy/auto_skip"_i18n,
    };
}

int getSponsorBlockStrategySetting(SettingItem item) {
    auto& conf          = ProgramConfig::instance();
    const auto& itemKey = ProgramConfig::SETTING_MAP[item].key;
    if (conf.setting.contains(itemKey)) {
        return conf.getSettingItem<int>(item, SPONSOR_BLOCK_OFF);
    }

    const auto& defaultKey = ProgramConfig::SETTING_MAP[SettingItem::SPONSOR_BLOCK_DEFAULT_STRATEGY].key;
    if (item != SettingItem::SPONSOR_BLOCK_DEFAULT_STRATEGY && conf.setting.contains(defaultKey)) {
        return conf.getSettingItem<int>(SettingItem::SPONSOR_BLOCK_DEFAULT_STRATEGY, SPONSOR_BLOCK_OFF);
    }

    return conf.getBoolOption(SettingItem::SPONSOR_BLOCK) ? SPONSOR_BLOCK_AUTO_SKIP : SPONSOR_BLOCK_OFF;
}

void initSponsorBlockSelector(BiliSelectorCell* selector, const std::string& title, SettingItem item) {
    selector->init(title, sponsorBlockStrategyLabels(), getSponsorBlockStrategySetting(item), [item](int data) {
        ProgramConfig::instance().setSettingItem(item, data);
        BasePlayerActivity::SPONSOR_BLOCK = BasePlayerActivity::hasSponsorBlockEnabledCategory();
        return true;
    });
}

}  // namespace

SettingSponsorBlock::SettingSponsorBlock() {
    this->inflateFromXMLRes("xml/fragment/setting_sponsor_block.xml");
    brls::Logger::debug("Fragment SettingSponsorBlock: create");

    auto& conf = ProgramConfig::instance();
    btnSponsorBlockServer->init(
        "wiliwili/setting/app/sponsor_block/server"_i18n,
        conf.getSettingItem(SettingItem::SPONSOR_BLOCK_SERVER, std::string{SPONSOR_BLOCK_DEFAULT_SERVER}),
        [](const std::string& data) {
            ProgramConfig::instance().setSettingItem(SettingItem::SPONSOR_BLOCK_SERVER, pystring::strip(data));
        },
        "wiliwili/setting/app/sponsor_block/server_hint"_i18n,
        "wiliwili/setting/app/sponsor_block/server_hint"_i18n, 128);

    initSponsorBlockSelector(selectorSponsorBlock, "wiliwili/setting/app/playback/sponsor_block"_i18n,
                             SettingItem::SPONSOR_BLOCK_DEFAULT_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockSponsor,
                             "wiliwili/setting/app/playback/sponsor_block_category/sponsor"_i18n,
                             SettingItem::SPONSOR_BLOCK_SPONSOR_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockSelfpromo,
                             "wiliwili/setting/app/playback/sponsor_block_category/selfpromo"_i18n,
                             SettingItem::SPONSOR_BLOCK_SELFPROMO_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockExclusiveAccess,
                             "wiliwili/setting/app/playback/sponsor_block_category/exclusive_access"_i18n,
                             SettingItem::SPONSOR_BLOCK_EXCLUSIVE_ACCESS_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockInteraction,
                             "wiliwili/setting/app/playback/sponsor_block_category/interaction"_i18n,
                             SettingItem::SPONSOR_BLOCK_INTERACTION_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockPoiHighlight,
                             "wiliwili/setting/app/playback/sponsor_block_category/poi_highlight"_i18n,
                             SettingItem::SPONSOR_BLOCK_POI_HIGHLIGHT_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockIntro,
                             "wiliwili/setting/app/playback/sponsor_block_category/intro"_i18n,
                             SettingItem::SPONSOR_BLOCK_INTRO_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockOutro,
                             "wiliwili/setting/app/playback/sponsor_block_category/outro"_i18n,
                             SettingItem::SPONSOR_BLOCK_OUTRO_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockPreview,
                             "wiliwili/setting/app/playback/sponsor_block_category/preview"_i18n,
                             SettingItem::SPONSOR_BLOCK_PREVIEW_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockFiller,
                             "wiliwili/setting/app/playback/sponsor_block_category/filler"_i18n,
                             SettingItem::SPONSOR_BLOCK_FILLER_STRATEGY);
    initSponsorBlockSelector(selectorSponsorBlockMusicOfftopic,
                             "wiliwili/setting/app/playback/sponsor_block_category/music_offtopic"_i18n,
                             SettingItem::SPONSOR_BLOCK_MUSIC_OFFTOPIC_STRATEGY);
}

SettingSponsorBlock::~SettingSponsorBlock() { brls::Logger::debug("Fragment SettingSponsorBlock: delete"); }

brls::View* SettingSponsorBlock::create() { return new SettingSponsorBlock(); }
