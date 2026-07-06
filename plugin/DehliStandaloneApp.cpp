// Custom Standalone application for the Dehli Musikk plugins.
//
// JUCE's built-in StandaloneFilterApp writes its settings file
// (<PluginName>.settings — audio-device config + window/plugin state) straight into the
// Application Support ROOT on macOS (its default folderName is ""). This is a near-verbatim
// copy that nests it under a "DehliMusikk" folder instead, so every product keeps its files
// (sample packs + standalone settings) under one ~/Library/Application Support/DehliMusikk/.
//
// It's compiled ONLY into each plugin's <Target>_Standalone target, with
// JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 defined (see dmse_setup_plugin in packaging/
// dmse_pack.cmake) — which makes JUCE skip its own app and use our juce_CreateApplication().
// StandaloneFilterApp is `final`, so we copy it rather than subclass. Preamble mirrors JUCE's
// juce_audio_plugin_client_Standalone.cpp so it builds identically in the same target.

#if JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_core/system/juce_TargetPlatform.h>
#include <juce_audio_plugin_client/detail/juce_CheckSettingMacros.h>
#include <juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h>
#include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
#include <juce_gui_basics/native/juce_WindowsHooks_windows.h>
#include <juce_audio_plugin_client/detail/juce_PluginUtilities.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace juce
{

class DehliStandaloneApp final : public JUCEApplication
{
public:
    DehliStandaloneApp()
    {
        PropertiesFile::Options options;

        options.applicationName     = CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config/DehliMusikk";
       #else
        options.folderName          = "DehliMusikk";   // was "" → settings landed in the Application Support root
       #endif

        appProperties.setStorageParameters (options);
    }

    const String getApplicationName() override              { return CharPointer_UTF8 (JucePlugin_Name); }
    const String getApplicationVersion() override           { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override              { return true; }
    void anotherInstanceStarted (const String&) override    {}

    virtual StandaloneFilterWindow* createWindow()
    {
        if (Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            jassertfalse;   // no displays → no window
            return nullptr;
        }

        return new StandaloneFilterWindow (getApplicationName(),
                                           LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                                           createPluginHolder());
    }

    virtual std::unique_ptr<StandalonePluginHolder> createPluginHolder()
    {
       #ifdef JucePlugin_PreferredChannelConfigurations
        constexpr StandalonePluginHolder::PluginInOuts channels[] { JucePlugin_PreferredChannelConfigurations };
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig (channels, juce::numElementsInArray (channels));
       #else
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig;
       #endif

        return std::make_unique<StandalonePluginHolder> (appProperties.getUserSettings(),
                                                         false, String{}, nullptr, channelConfig, false);
    }

    void initialise (const String&) override
    {
        mainWindow = rawToUniquePtr (createWindow());

        if (mainWindow != nullptr)
        {
           #if JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE
            Desktop::getInstance().setKioskModeComponent (mainWindow.get(), false);
           #endif
            mainWindow->setVisible (true);
        }
        else
        {
            pluginHolder = createPluginHolder();
        }
    }

    void shutdown() override
    {
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (pluginHolder != nullptr)
            pluginHolder->savePluginState();

        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            Timer::callAfterDelay (100, []()
            {
                if (auto app = JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

protected:
    ApplicationProperties appProperties;
    std::unique_ptr<StandaloneFilterWindow> mainWindow;

private:
    std::unique_ptr<StandalonePluginHolder> pluginHolder;
};

} // namespace juce

// JUCE's standalone main() (still provided by the plugin client) calls this to build the app.
juce::JUCEApplicationBase* juce_CreateApplication() { return new juce::DehliStandaloneApp(); }

#endif  // JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
