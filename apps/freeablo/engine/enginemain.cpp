#include <iostream>
#include <thread>
#include <functional>
#include <boost/asio.hpp>
#include <misc/misc.h>
#include <input/inputmanager.h>
#include <enet/enet.h>
#include "../faworld/world.h"
#include "../faworld/player.h"
#include "../falevelgen/levelgen.h"
#include "../falevelgen/random.h"
#include "../fagui/guimanager.h"
#include "../faaudio/audiomanager.h"
#include "../faworld/itemmanager.h"
#include "../faworld/playerfactory.h"
#include "../fasavegame/gameloader.h"
#include <serial/textstream.h>
#include "threadmanager.h"
#include "enginemain.h"

namespace bpo = boost::program_options;


namespace Engine
{
    volatile bool renderDone = false;

    EngineInputManager& EngineMain::inputManager()
    {
        return *(mInputManager.get());
    }

    void EngineMain::run(const bpo::variables_map& variables)
    {
        Settings::Settings settings;
        if(!settings.loadUserSettings())
            return;

        size_t resolutionWidth = settings.get<size_t>("Display","resolutionWidth");
        size_t resolutionHeight = settings.get<size_t>("Display","resolutionHeight");
        std::string fullscreen = settings.get<std::string>("Display", "fullscreen");
        std::string pathEXE = settings.get<std::string>("Game", "PathEXE");
        if (pathEXE == "")
        {
            pathEXE = "Diablo.exe";
        }

        Engine::ThreadManager threadManager;
        FARender::Renderer renderer(resolutionWidth, resolutionHeight, fullscreen == "true");
        mInputManager = std::make_shared<EngineInputManager>(renderer.getNuklearContext());
        mInputManager->registerKeyboardObserver(this);
        std::thread mainThread(std::bind(&EngineMain::runGameLoop, this, &variables, pathEXE));
        threadManager.run();
        renderDone = true;

        mainThread.join();
    }

    void EngineMain::runGameLoop(const bpo::variables_map& variables, const std::string& pathEXE)
    {
        FALevelGen::FAsrand(static_cast<int> (time(nullptr)));

        FAWorld::Player* player = nullptr;
        FARender::Renderer& renderer = *FARender::Renderer::get();

        Settings::Settings settings;
        if(!settings.loadUserSettings())
            return;

        std::string characterClass = variables["character"].as<std::string>();

        DiabloExe::DiabloExe exe(pathEXE);
        if (!exe.isLoaded())
        {
            renderer.stop();
            return;
        }

        FAWorld::ItemManager& itemManager = FAWorld::ItemManager::get();

        std::unique_ptr<FAWorld::World> worldPtr;

        bool isServer = true;
        bool clientWaitingForLevel = false;
        FAWorld::PlayerFactory playerFactory(exe);

        //std::unique_ptr<FAWorld::World> worldPtr(new FAWorld::World(exe));

        FILE* f = fopen("save.sav", "rb");

        if (f)
        {
            fseek(f, 0, SEEK_END);
            size_t size = ftell(f);
            fseek(f, 0, SEEK_SET);

            std::string tmp;
            tmp.resize(size);

            fread((void*)tmp.data(), 1, size, f);

            Serial::TextReadStream stream(tmp);
            FASaveGame::GameLoader loader(stream);

            worldPtr.reset(new FAWorld::World(loader, exe));

            player = worldPtr->getCurrentPlayer();
        }
        else
        {
            worldPtr.reset(new FAWorld::World(exe));

            worldPtr->generateLevels();

            itemManager.loadItems(&exe);
            player = playerFactory.create(characterClass);
            worldPtr->addCurrentPlayer(player);

            if (variables["invuln"].as<std::string>() == "on")
                player->setInvuln(true);

            int32_t currentLevel = variables["level"].as<int32_t>();

            if (currentLevel == -1)
                currentLevel = 0;

            // -1 represents the main menu
            if(currentLevel != -1 && isServer)
                worldPtr->setLevel(currentLevel);
            else
                clientWaitingForLevel = true;
        }

        FAWorld::World& world = *worldPtr.get();

        FAGui::GuiManager guiManager(*this, *player);
        world.setGuiManager (&guiManager);

        mInputManager->setGuiManager (&guiManager);
        mInputManager->registerKeyboardObserver(&world);
        mInputManager->registerMouseObserver(&world);

        boost::asio::io_service io;

        // Main game logic loop
        while(!mDone)
        {
            boost::asio::deadline_timer timer(io, boost::posix_time::milliseconds(1000/FAWorld::World::ticksPerSecond));

            if (clientWaitingForLevel)
            {
                clientWaitingForLevel = world.getCurrentLevel() != nullptr;
            }

            mInputManager->update(mPaused);
            if(!mPaused && !clientWaitingForLevel)
            {
                world.update(mNoclip);
            }

            nk_context* ctx = renderer.getNuklearContext();
            guiManager.update(mPaused, ctx);



            FAWorld::GameLevel* level = world.getCurrentLevel();
            FARender::RenderState* state = renderer.getFreeState();
            if(state)
            {
                state->mPos = player->getPos();
                if(level != NULL)
                    state->tileset = renderer.getTileset(*level);
                state->level = level;
                if(!FAGui::cursorPath.empty())
                    state->mCursorEmpty = false;
                else
                    state->mCursorEmpty = true;
                state->mCursorFrame = FAGui::cursorFrame;
                state->mCursorSpriteGroup = renderer.loadImage("data/inv/objcurs.cel");
                state->mCursorHotspot = FAGui::cursorHotspot;
                world.fillRenderState(state);
                state->nuklearData.fill(ctx);
            }

            std::vector<uint32_t> spritesToPreload;
            if (renderer.getAndClearSpritesNeedingPreloading(spritesToPreload))
                ThreadManager::get()->sendSpritesForPreload(spritesToPreload);

            nk_clear(ctx);

            renderer.setCurrentState(state);

            auto remainingTickTime = timer.expires_from_now().total_milliseconds();

            if(remainingTickTime < 0)
                std::cerr << "tick time exceeded by " << -remainingTickTime << "ms" << std::endl;

            timer.wait();
        }

        renderer.stop();
        renderer.waitUntilDone();
    }

    void EngineMain::notify(KeyboardInputAction action)
    {
        if (action == PAUSE)
        {
            togglePause();
        }
        if(action == QUIT)
        {
            stop();
        }
        else if(action == NOCLIP)
        {
            toggleNoclip();
        }
    }

    void EngineMain::stop()
    {
        mDone = true;
    }

    void EngineMain::togglePause()
    {
        mPaused = !mPaused;
    }

    void EngineMain::toggleNoclip()
    {
        mNoclip = !mNoclip;
    }
}
