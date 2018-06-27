#include <log.h>
#include <mcpelauncher/minecraft_utils.h>
#include <mcpelauncher/crash_handler.h>
#include <mcpelauncher/path_helper.h>
#include <minecraft/Common.h>
#include <mcpelauncher/app_platform.h>
#include <minecraft/Whitelist.h>
#include <minecraft/OpsList.h>
#include <minecraft/Api.h>
#include <minecraft/LevelSettings.h>
#include <minecraft/FilePathManager.h>
#include <minecraft/AppResourceLoader.h>
#include <minecraft/MinecraftEventing.h>
#include <minecraft/ResourcePack.h>
#include <minecraft/ResourcePackStack.h>
#include <minecraft/SaveTransactionManager.h>
#include <minecraft/AutomationClient.h>
#include <minecraft/ExternalFileLevelStorageSource.h>
#include <minecraft/ServerInstance.h>
#include <minecraft/Minecraft.h>
#include <minecraft/I18n.h>
#include <minecraft/DedicatedServerCommandOrigin.h>
#include <minecraft/MinecraftCommands.h>
#include <mcpelauncher/mod_loader.h>
#include <argparser.h>
#include <hybris/dlfcn.h>
#include "launcher_minecraft_api.h"
#include "stub_key_provider.h"
#include "server_properties.h"
#include "server_minecraft_app.h"
#include "console_reader.h"

int main(int argc, char *argv[]) {
    CrashHandler::registerCrashHandler();
    MinecraftUtils::workaroundLocaleBug();

    argparser::arg_parser p;
    argparser::arg<std::string> gameDir (p, "--game-dir", "-dg", "Directory with the game and assets");
    argparser::arg<std::string> dataDir (p, "--data-dir", "-dd", "Directory to use for the data");
    argparser::arg<std::string> cacheDir (p, "--cache-dir", "-dc", "Directory to use for cache");
    if (!p.parse(argc, (const char**) argv))
        return 1;
    if (!gameDir.get().empty())
        PathHelper::setGameDir(gameDir);
    if (!dataDir.get().empty())
        PathHelper::setDataDir(dataDir);
    if (!cacheDir.get().empty())
        PathHelper::setCacheDir(cacheDir);

    MinecraftUtils::setupForHeadless();

    Log::trace("Launcher", "Loading Minecraft library");
    void* handle = MinecraftUtils::loadMinecraftLib();
    Log::info("Launcher", "Loaded Minecraft library");

    Log::debug("Launcher", "Minecraft is at offset 0x%x", MinecraftUtils::getLibraryBase(handle));
    MinecraftUtils::initSymbolBindings(handle);
    Log::info("Launcher", "Game version: %s", Common::getGameVersionStringNet().c_str());

    Log::info("Launcher", "Applying patches");
    void* ptr = hybris_dlsym(handle, "_ZN5Level17_checkUserStorageEv");
    PatchUtils::patchCallInstruction(ptr, (void*) (void (*)()) []{ }, true);

    ModLoader modLoader;
    modLoader.loadModsFromDirectory(PathHelper::getPrimaryDataDirectory() + "mods/");

    Log::trace("Launcher", "Initializing AppPlatform (vtable)");
    LauncherAppPlatform::initVtable(handle);
    Log::trace("Launcher", "Initializing AppPlatform (create instance)");
    std::unique_ptr<LauncherAppPlatform> appPlatform (new LauncherAppPlatform());
    Log::trace("Launcher", "Initializing AppPlatform (initialize call)");
    appPlatform->initialize();

    Log::trace("Launcher", "Loading server properties");
    ServerProperties props;
    props.load();

    Log::trace("Launcher", "Loading whitelist and operator list");
    Whitelist whitelist;
    OpsList ops (true);
    Log::trace("Launcher", "Initializing Minecraft API classes");
    LauncherMinecraftApi api (handle);

    Log::trace("Launcher", "Setting up level settings");
    LevelSettings levelSettings;
    levelSettings.seed = LevelSettings::parseSeedString(props.worldSeed.get(), Level::createRandomSeed());
    levelSettings.gametype = props.gamemode;
    levelSettings.forceGameType = props.forceGamemode;
    levelSettings.difficulty = props.difficulty;
    levelSettings.dimension = 0;
    levelSettings.generator = props.worldGenerator;
    levelSettings.edu = false;
    levelSettings.mpGame = true;
    levelSettings.lanBroadcast = true;
    levelSettings.commandsEnabled = true;
    levelSettings.texturepacksRequired = false;

    Log::trace("Launcher", "Initializing FilePathManager");
    FilePathManager pathmgr (appPlatform->getCurrentStoragePath(), false);
    pathmgr.setPackagePath(appPlatform->getPackagePath());
    pathmgr.setSettingsPath(pathmgr.getRootPath());
    Log::trace("Launcher", "Initializing resource loaders");
    Resource::registerLoader((ResourceFileSystem) 1, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return pathmgr.getPackagePath(); })));
    // Resource::registerLoader((ResourceFileSystem) 7, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return pathmgr.getDataUrl(); })));
    Resource::registerLoader((ResourceFileSystem) 8, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return pathmgr.getUserDataPath(); })));
    Resource::registerLoader((ResourceFileSystem) 4, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return pathmgr.getSettingsPath(); })));
    // Resource::registerLoader((ResourceFileSystem) 5, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return pathmgr.getExternalFilePath(); })));
    // Resource::registerLoader((ResourceFileSystem) 2, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return ""; })));
    // Resource::registerLoader((ResourceFileSystem) 3, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return ""; })));
    // Resource::registerLoader((ResourceFileSystem) 9, std::unique_ptr<ResourceLoader>(new ScreenshotLoader));
    // Resource::registerLoader((ResourceFileSystem) 0xA, std::unique_ptr<ResourceLoader>(new AppResourceLoader([&pathmgr] { return ""; })));

    Log::trace("Launcher", "Initializing MinecraftEventing (create instance)");
    MinecraftEventing eventing (pathmgr.getRootPath());
    /*Log::trace("Launcher", "Social::UserManager::CreateUserManager()");
    auto userManager = Social::UserManager::CreateUserManager();*/
    Log::trace("Launcher", "Initializing MinecraftEventing (init call)");
    eventing.init();
    Log::trace("Launcher", "Initializing ResourcePackManager");
    ContentTierManager ctm;
    ResourcePackManager* resourcePackManager = new ResourcePackManager([&pathmgr]() { return pathmgr.getRootPath(); }, ctm);
    Resource::registerLoader((ResourceFileSystem) 0, std::unique_ptr<ResourceLoader>(resourcePackManager));
    Log::trace("Launcher", "Initializing PackManifestFactory");
    PackManifestFactory packManifestFactory (eventing);
    Log::trace("Launcher", "Initializing SkinPackKeyProvider");
    SkinPackKeyProvider skinPackKeyProvider;
    Log::trace("Launcher", "Initializing StubKeyProvider");
    StubKeyProvider stubKeyProvider;
    Log::trace("Launcher", "Initializing PackSourceFactory");
    PackSourceFactory packSourceFactory (nullptr);
    Log::trace("Launcher", "Initializing ResourcePackRepository");
    ResourcePackRepository resourcePackRepo (eventing, packManifestFactory, skinPackKeyProvider, &pathmgr, packSourceFactory);
    Log::trace("Launcher", "Adding vanilla resource pack");
    std::unique_ptr<ResourcePackStack> stack (new ResourcePackStack());
    stack->add(PackInstance(resourcePackRepo.vanillaPack, -1, false, nullptr), resourcePackRepo, false);
    resourcePackManager->setStack(std::move(stack), (ResourcePackStackType) 3, false);
    Log::trace("Launcher", "Adding world resource packs");
    resourcePackRepo.addWorldResourcePacks(pathmgr.getWorldsPath().std() + props.worldDir.get());
    resourcePackRepo.refreshPacks();
    Log::trace("Launcher", "Initializing Automation::AutomationClient");
    DedicatedServerMinecraftApp minecraftApp;
    Automation::AutomationClient aclient (minecraftApp);
    minecraftApp.automationClient = &aclient;
    Log::debug("Launcher", "Initializing SaveTransactionManager");
    std::shared_ptr<SaveTransactionManager> saveTransactionManager (new SaveTransactionManager([](bool b) {
        if (b)
            Log::debug("Launcher", "Saving the world...");
        else
            Log::debug("Launcher", "World has been saved.");
    }));
    Log::debug("Launcher", "Initializing ExternalFileLevelStorageSource");
    ExternalFileLevelStorageSource levelStorage (&pathmgr, saveTransactionManager);
    Log::debug("Launcher", "Initializing ServerInstance");
    auto idleTimeout = std::chrono::seconds((int) (props.playerIdleTimeout * 60.f));
    IContentKeyProvider* keyProvider = &stubKeyProvider;
    auto createLevelStorageFunc = [&levelStorage, &props, keyProvider](Scheduler& scheduler) {
        return levelStorage.createLevelStorage(scheduler, props.worldDir.get(), mcpe::string(), *keyProvider);
    };
    ServerInstance instance (minecraftApp, whitelist, ops, &pathmgr, idleTimeout, props.worldDir.get(), props.worldName.get(), props.motd.get(), levelSettings, api, props.viewDistance, true, props.port, props.portV6, props.maxPlayers, props.onlineMode, {}, "normal", *mce::UUID::EMPTY, eventing, resourcePackRepo, ctm, *resourcePackManager, createLevelStorageFunc, pathmgr.getWorldsPath(), nullptr, nullptr, [](mcpe::string const& s) {
        Log::debug("Launcher", "Unloading level: %s", s.c_str());
    }, [](mcpe::string const& s) {
        Log::debug("Launcher", "Saving level: %s", s.c_str());
    });
    Log::trace("Launcher", "Loading language data");
    I18n::loadLanguages(*resourcePackManager, "en_US");
    resourcePackManager->onLanguageChanged();
    Log::info("Launcher", "Server initialized");
    modLoader.onServerInstanceInitialized(&instance);
    instance.startServerThread();

    ConsoleReader reader;
    ConsoleReader::registerInterruptHandler();

    std::string line;
    while (reader.read(line)) {
        instance.queueForServerThread([&instance, line]() {
            std::unique_ptr<DedicatedServerCommandOrigin> commandOrigin(new DedicatedServerCommandOrigin("Server", *instance.minecraft));
            instance.minecraft->getCommands()->requestCommandExecution(std::move(commandOrigin), line, 4, true);
        });
    }

    Log::info("Launcher", "Stopping...");
    instance.leaveGameSync();

    MinecraftUtils::workaroundShutdownCrash(handle);
    return 0;
}
