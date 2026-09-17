#include "app/GameDef.h"
#include "platform/Paths.h"
#include "tinyxml2.h"

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

namespace {

// The original calls QueryIntAttribute with pointers into packed 1-2 byte fields, so each 4-byte write
// clobbers the next field and the fields effectively keep the low byte / low 16 bits. Reading in the same
// order and truncating the same way gives identical results.
void QueryBool(const XMLElement* e, const char* attr, bool& out) {
    int v = out ? 1 : 0;
    if (e && e->QueryIntAttribute(attr, &v) == tinyxml2::XML_SUCCESS)
        out = (v & 0xFF) != 0;
}
void QueryShort(const XMLElement* e, const char* attr, int16_t& out) {
    int v = out;
    if (e && e->QueryIntAttribute(attr, &v) == tinyxml2::XML_SUCCESS)
        out = static_cast<int16_t>(v & 0xFFFF);
}
void QueryString(const XMLElement* e, const char* attr, std::string& out) {
    const char* v = e ? e->Attribute(attr) : nullptr;
    out.assign(v ? v : "");  // original: std::string::assign(Attribute(...)) — null attribute -> empty
}
const XMLElement* Path(const XMLDocument& doc, std::initializer_list<const char*> names) {
    const XMLElement* e = doc.FirstChildElement("carbon");
    for (const char* n : names) {
        if (!e) return nullptr;
        e = e->FirstChildElement(n);
    }
    return e;
}

}  // namespace

bool GameDef::Load() {
    static XMLDocument doc;  // original: member at +0x2f8
    doc.LoadFile(Paths::Resolve("rom:/gamedef.xml").c_str());

    const XMLElement* carbon = doc.FirstChildElement("carbon");
    QueryString(carbon, "packageName", packageName);

    const XMLElement* titlesEl = carbon ? carbon->FirstChildElement("titles") : nullptr;
    int count = 0;
    if (titlesEl) titlesEl->QueryIntAttribute("count", &count);
    const XMLElement* game = titlesEl ? titlesEl->FirstChildElement("game") : nullptr;
    // The original trusts `count` and would dereference a null element if there are fewer <game> nodes.
    for (int i = 0; i < count && game; ++i) {
        GameEntry entry;
        game->QueryIntAttribute("id", &entry.id);
        QueryString(game, "name", entry.name);
        QueryString(game, "engine", entry.engine);
        QueryString(game, "romName", entry.romName);
        QueryString(game, "previewImage", entry.previewImage);
        QueryString(game, "description", entry.description);
        titles.push_back(entry);
        game = game->NextSiblingElement("game");
    }

    QueryString(Path(doc, {"assets", "intro", "Splash1"}), "assetName", splash1);
    QueryString(Path(doc, {"assets", "intro", "Splash2"}), "assetName", splash2);
    QueryString(Path(doc, {"assets", "common", "Button_A"}), "assetName", buttonA);
    const XMLElement* bb = Path(doc, {"assets", "common", "Button_B"});
    QueryString(bb, "assetName", buttonB);
    QueryBool(bb, "enabled", buttonBEnabled);
    QueryString(Path(doc, {"assets", "common", "SelectIcon"}), "assetName", selectIcon);

    const XMLElement* bg = Path(doc, {"assets", "mainMenu", "MenuBackground"});
    QueryString(bg, "assetName", menuBackground);
    QueryBool(bg, "enabled", menuBackgroundEnabled);
    QueryBool(bg, "scrolling", menuBackgroundScrolling);
    QueryString(Path(doc, {"assets", "mainMenu", "MenuBar"}), "assetName", mainMenuBar);
    QueryString(Path(doc, {"assets", "mainMenu", "DescriptionBar"}), "assetName", descriptionBar);
    QueryString(Path(doc, {"assets", "mainMenu", "font"}), "name", font);
    QueryString(Path(doc, {"assets", "mainMenu", "fontSmall"}), "name", fontSmall);
    const XMLElement* sprite = Path(doc, {"assets", "mainMenu", "MenuSprite"});
    QueryString(sprite, "assetName", menuSprite);
    QueryBool(sprite, "enabled", menuSpriteEnabled);
    QueryShort(sprite, "width", menuSpriteWidth);
    QueryShort(sprite, "height", menuSpriteHeight);
    const XMLElement* logo = Path(doc, {"assets", "mainMenu", "GameLogo"});
    QueryString(logo, "assetName", gameLogo);
    QueryBool(logo, "enabled", gameLogoEnabled);

    const XMLElement* pmi = Path(doc, {"assets", "inGameMenu", "PauseMenuImage"});
    QueryString(pmi, "assetName", pauseMenuImage);
    QueryBool(pmi, "enabled", pauseMenuImageEnabled);
    const XMLElement* side = Path(doc, {"assets", "inGameMenu", "SidePauseMenu"});
    QueryString(side, "assetName", sidePauseMenu);
    QueryBool(side, "enabled", sidePauseMenuEnabled);
    const XMLElement* top = Path(doc, {"assets", "inGameMenu", "TopPauseMenu"});
    QueryString(top, "assetName", topPauseMenu);
    QueryBool(top, "enabled", topPauseMenuEnabled);
    const XMLElement* igb = Path(doc, {"assets", "inGameMenu", "InGameBackground"});
    QueryString(igb, "assetName", inGameBackground);
    QueryBool(igb, "enabled", inGameBackgroundEnabled);
    QueryString(Path(doc, {"assets", "inGameMenu", "SaveStateBox"}), "assetName", saveStateBox);
    QueryString(Path(doc, {"assets", "inGameMenu", "DialogBox"}), "assetName", dialogBox);
    QueryString(Path(doc, {"assets", "inGameMenu", "MenuBar"}), "assetName", inGameMenuBar);
    QueryString(Path(doc, {"assets", "inGameMenu", "ControlImage"}), "assetName", controlImage);

    QueryString(Path(doc, {"assets", "creditsMenu", "splashData"}), "document", creditsDocument);
    return true;
}
