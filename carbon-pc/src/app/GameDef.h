// GameDef — rom:/gamedef.xml (tinyxml2). Original: ctor @ 0x71000a1330, Load @ 0x71000a1720,
// accessors @ 0x71000a2330 (credits doc), 0x71000a2340 (title count), 0x71000a2360 (copy of titles),
// 0x71000a2450 (font), 0x71000a2460 (fontSmall). Object size 0x690; field offsets noted below.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct GameEntry {                  // 0x80 bytes
    int id = 0;                     // +0x00
    std::string engine;             // +0x08 (parsed, never used: the core is chosen by extension/header)
    std::string name;               // +0x20
    std::string romName;            // +0x38
    std::string previewImage;       // +0x50
    std::string description;        // +0x68
};

class GameDef {
public:
    std::string buttonA;            // +0x000 common/Button_A@assetName
    std::string buttonB;            // +0x020 common/Button_B@assetName
    bool buttonBEnabled = false;    // +0x038 common/Button_B@enabled (gates the B-button hint on the main menu)
    std::string splash1;            // +0x100 intro/Splash1
    std::string splash2;            // +0x118 intro/Splash2 (loaded, never drawn)
    std::string selectIcon;         // +0x130
    std::string menuBackground;     // +0x148
    std::string mainMenuBar;        // +0x160
    std::string menuSprite;         // +0x190
    std::string gameLogo;           // +0x1a8
    std::string descriptionBar;     // +0x1c0
    bool menuBackgroundEnabled = false;    // +0x1d8
    bool menuBackgroundScrolling = false;  // +0x1d9
    bool menuSpriteEnabled = false;        // +0x1dc
    int16_t menuSpriteWidth = 0;           // +0x1de
    int16_t menuSpriteHeight = 0;          // +0x1e0
    bool gameLogoEnabled = false;          // +0x1e2
    std::string pauseMenuImage;     // +0x1e8 inGameMenu/PauseMenuImage   -> texture "PauseMenuText"
    std::string sidePauseMenu;      // +0x200 inGameMenu/SidePauseMenu    -> texture "PauseMenu"
    std::string topPauseMenu;       // +0x218 inGameMenu/TopPauseMenu     -> texture "PauseTopMenu"
    std::string inGameBackground;   // +0x230 inGameMenu/InGameBackground -> texture "GameBackground"
    std::string inGameMenuBar;      // +0x290 inGameMenu/MenuBar          -> texture "PauseMenuBar"
    std::string saveStateBox;       // +0x2a8
    std::string dialogBox;          // +0x2c0
    std::string controlImage;       // +0x2d8 inGameMenu/ControlImage     -> texture "ControllerImage"
    bool pauseMenuImageEnabled = false;    // +0x2f0
    bool sidePauseMenuEnabled = false;     // +0x2f1
    bool topPauseMenuEnabled = false;      // +0x2f2
    bool inGameBackgroundEnabled = false;  // +0x2f3
    // +0x2f8 tinyxml2::XMLDocument (kept alive for the object's lifetime in the original)
    std::string font;               // +0x600 mainMenu/font@name
    std::string fontSmall;          // +0x618 mainMenu/fontSmall@name
    std::string packageName;        // +0x630 carbon@packageName
    std::string creditsDocument;    // +0x660 creditsMenu/splashData@document
    std::vector<GameEntry> titles;  // +0x678

    bool Load();
    int GetTitleCount() const { return static_cast<int>(titles.size()); }
};
