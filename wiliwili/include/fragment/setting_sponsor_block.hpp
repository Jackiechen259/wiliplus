#pragma once

#include <borealis/core/bind.hpp>
#include <borealis/core/box.hpp>

namespace brls {
class InputCell;
}  // namespace brls

class BiliSelectorCell;

class SettingSponsorBlock : public brls::Box {
public:
    SettingSponsorBlock();

    ~SettingSponsorBlock() override;

    static brls::View* create();

private:
    BRLS_BIND(brls::InputCell, btnSponsorBlockServer, "setting/sponsor_block/server");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlock, "setting/video/sponsor_block");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockSponsor, "setting/video/sponsor_block/sponsor");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockSelfpromo, "setting/video/sponsor_block/selfpromo");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockExclusiveAccess, "setting/video/sponsor_block/exclusive_access");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockInteraction, "setting/video/sponsor_block/interaction");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockPoiHighlight, "setting/video/sponsor_block/poi_highlight");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockIntro, "setting/video/sponsor_block/intro");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockOutro, "setting/video/sponsor_block/outro");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockPreview, "setting/video/sponsor_block/preview");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockFiller, "setting/video/sponsor_block/filler");
    BRLS_BIND(BiliSelectorCell, selectorSponsorBlockMusicOfftopic, "setting/video/sponsor_block/music_offtopic");
};
