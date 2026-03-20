#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <vector>
#include <optional>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdint>

using namespace geode::prelude;

// ════════════════════════════════════════════════════════════════════════════
//  Input struct
// ════════════════════════════════════════════════════════════════════════════
struct BotInput {
    int  frame   = 0;
    int  button  = 1;
    bool player2 = false;
    bool down    = true;
};

// ════════════════════════════════════════════════════════════════════════════
//  Bot state
// ════════════════════════════════════════════════════════════════════════════
enum class BotState { Disabled, Record, Playback };

// ════════════════════════════════════════════════════════════════════════════
//  Bot singleton
// ════════════════════════════════════════════════════════════════════════════
class Bot {
public:
    static Bot& get() {
        static Bot s;
        return s;
    }

    // ── State ─────────────────────────────────────────────────────────────────
    BotState state = BotState::Disabled;
    float    framerate = 240.f;

    bool isDisabled()  const { return state == BotState::Disabled; }
    bool isRecording() const { return state == BotState::Record;   }
    bool isPlaying()   const { return state == BotState::Playback; }

    // ── Inputs ────────────────────────────────────────────────────────────────
    std::vector<BotInput> inputs;
    int playbackIdx = 0;

    size_t getInputCount() const { return inputs.size(); }
    bool   isEmpty()       const { return inputs.empty(); }

    void clearInputs() {
        inputs.clear();
        playbackIdx = 0;
    }

    void restart() {
        playbackIdx = 0;
    }

    // ── Recording ─────────────────────────────────────────────────────────────
    void recordInput(int frame, int button, bool player2, bool down) {
        inputs.push_back({ frame, button, player2, down });
    }

    void removeInputsAfter(int frame) {
        inputs.erase(
            std::remove_if(inputs.begin(), inputs.end(),
                [frame](const BotInput& i){ return i.frame > frame; }),
            inputs.end()
        );
    }

    // ── Playback ──────────────────────────────────────────────────────────────
    // Returns next input if it should fire at this frame, otherwise nullopt
    std::optional<BotInput> poll(int frame) {
        if (playbackIdx >= (int)inputs.size()) return std::nullopt;
        const auto& next = inputs[playbackIdx];
        if (next.frame <= frame) {
            ++playbackIdx;
            return next;
        }
        return std::nullopt;
    }

    // ── Save ──────────────────────────────────────────────────────────────────
    bool save(const std::filesystem::path& path) {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        const char magic[] = "ABOT";
        f.write(magic, 4);
        f.write(reinterpret_cast<const char*>(&framerate), sizeof(framerate));

        uint32_t count = static_cast<uint32_t>(inputs.size());
        f.write(reinterpret_cast<const char*>(&count), sizeof(count));
        f.write(reinterpret_cast<const char*>(inputs.data()), count * sizeof(BotInput));

        return f.good();
    }

    // ── Load ──────────────────────────────────────────────────────────────────
    bool load(const std::filesystem::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        char magic[4];
        f.read(magic, 4);
        if (std::string(magic, 4) != "ABOT") return false;

        f.read(reinterpret_cast<char*>(&framerate), sizeof(framerate));

        uint32_t count = 0;
        f.read(reinterpret_cast<char*>(&count), sizeof(count));
        inputs.resize(count);
        f.read(reinterpret_cast<char*>(inputs.data()), count * sizeof(BotInput));

        playbackIdx = 0;
        return f.good();
    }

    // ── Checkpoint helpers ────────────────────────────────────────────────────
    bool dontPlaceAuto       = false;
    bool p1TriedCheckpoint   = false;
    bool p2TriedCheckpoint   = false;

private:
    Bot() = default;
};

// ── Helper: get current progress (GD 2.2081 counts 2x fast so divide by 2) ──
static int getProgress(GJBaseGameLayer* self) {
    return self->m_gameState.m_currentProgress / 2;
}

// ════════════════════════════════════════════════════════════════════════════
//  TPS bypass via CCScheduler
// ════════════════════════════════════════════════════════════════════════════
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        auto& bot = Bot::get();
        if (!bot.isDisabled() && PlayLayer::get()) {
            float tpsDt = 1.f / bot.framerate;
            CCScheduler::update(tpsDt);
        } else {
            CCScheduler::update(dt);
        }
    }
};

// ════════════════════════════════════════════════════════════════════════════
//  PlayLayer hooks
// ════════════════════════════════════════════════════════════════════════════
class $modify(BotPlayLayer, PlayLayer) {

    bool init(GJGameLevel* level, bool p1, bool p2) {
        if (!PlayLayer::init(level, p1, p2)) return false;

        auto& bot = Bot::get();
        // Store level info in framerate field (framerate set on record start)
        if (bot.isRecording()) {
            bot.framerate = 240.f; // default; user can set via settings
        }

        // Add floating bot button top-left
        auto winSize = CCDirector::sharedDirector()->getWinSize();

        auto* spr = ButtonSprite::create("Bot", "bigFont.fnt", "GJ_button_01.png", 0.45f);
        auto* btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(BotPlayLayer::onOpenMenu)
        );
        auto* menu = CCMenu::create(btn, nullptr);
        menu->setPosition({36.f, winSize.height - 36.f});
        menu->setZOrder(999);
        addChild(menu);

        return true;
    }

    void onOpenMenu(CCObject*) {
        // Simple notification-based state toggle on mobile
        auto& bot = Bot::get();
        if (bot.isDisabled()) {
            bot.clearInputs();
            bot.state = BotState::Record;
            Notification::create("Recording started!", NotificationIcon::Success)->show();
        } else if (bot.isRecording()) {
            bot.state = BotState::Playback;
            bot.restart();
            Notification::create(
                fmt::format("Playing back {} inputs!", bot.getInputCount()),
                NotificationIcon::Success
            )->show();
        } else {
            bot.state = BotState::Disabled;
            Notification::create("Bot disabled.", NotificationIcon::None)->show();
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& bot = Bot::get();
        Bot::get().dontPlaceAuto = false;

        // If no checkpoints, restart replay/recording from beginning
        if (m_checkpointArray->count() == 0) {
            if (!bot.isPlaying())
                bot.clearInputs();
            bot.restart();
        }
    }

    CheckpointObject* markCheckpoint() {
        auto& bot = Bot::get();
        // Don't mark checkpoints if recording and a player is dead
        if (bot.isRecording() && (m_player1->m_isDead || m_player2->m_isDead))
            return nullptr;
        return PlayLayer::markCheckpoint();
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        auto& bot = Bot::get();

        // When recording and restoring a checkpoint, trim inputs recorded
        // after the checkpoint's progress value
        if (bot.isRecording()) {
            int checkpointFrame = checkpoint->m_gameState.m_currentProgress / 2;
            bot.removeInputsAfter(checkpointFrame);
        }

        PlayLayer::loadFromCheckpoint(checkpoint);
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        auto& bot = Bot::get();
        if (bot.isRecording()) {
            // Auto-save on level complete
            auto dir = Mod::get()->getSaveDir() / "replays";
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec))
                std::filesystem::create_directories(dir, ec);
            bot.save(dir / "autosave.abot");
            bot.state = BotState::Disabled;
            Notification::create(
                fmt::format("Saved {} inputs!", bot.getInputCount()),
                NotificationIcon::Success
            )->show();
        }
    }

    void onQuit() {
        Bot::get().state = BotState::Disabled;
        PlayLayer::onQuit();
    }
};

// ════════════════════════════════════════════════════════════════════════════
//  PlayerObject — checkpoint placement fix (mirrors original Bot.cpp exactly)
// ════════════════════════════════════════════════════════════════════════════
class $modify(BotPlayerObject, PlayerObject) {
    struct Fields {
        bool triedPlacingCheckpoint = false;
    };

    void tryPlaceCheckpoint() {
        auto& bot = Bot::get();
        if (bot.dontPlaceAuto) {
            m_fields->triedPlacingCheckpoint = true;
            return;
        }
        PlayerObject::tryPlaceCheckpoint();
    }
};

// ════════════════════════════════════════════════════════════════════════════
//  GJBaseGameLayer — core recording and playback hooks
//  This is where 100% accuracy comes from.
//  Mirrors the original Bot.cpp logic exactly, adapted for 2.2081.
// ════════════════════════════════════════════════════════════════════════════
class $modify(BotBGLHook, GJBaseGameLayer) {

    // ── update: checkpoint placement fix (from original Bot.cpp) ─────────────
    void update(float dt) {
        auto& bot = Bot::get();

        auto* p1Fields = reinterpret_cast<BotPlayerObject*>(m_player1)->m_fields.self();
        auto* p2Fields = reinterpret_cast<BotPlayerObject*>(m_player2)->m_fields.self();

        p1Fields->triedPlacingCheckpoint = false;
        p2Fields->triedPlacingCheckpoint = false;

        bot.dontPlaceAuto = true;
        GJBaseGameLayer::update(dt);
        bot.dontPlaceAuto = false;

        // Re-fire checkpoint placement after update so it lands at the
        // correct physics step — exactly as the original does
        if (p1Fields->triedPlacingCheckpoint) {
            m_player1->m_shouldTryPlacingCheckpoint = true;
            m_player1->tryPlaceCheckpoint();
        }
        if (p2Fields->triedPlacingCheckpoint) {
            m_player2->m_shouldTryPlacingCheckpoint = true;
            m_player2->tryPlaceCheckpoint();
        }
    }

    // ── simulateClick: injects a click the same way the original does ─────────
    void simulateClick(PlayerButton button, bool down, bool player2) {
        auto performButton = down
            ? &PlayerObject::pushButton
            : &PlayerObject::releaseButton;

        bool swapControls = GameManager::get()->getGameVariable("0010");
        player2 = swapControls ? !player2 : player2;

        if (m_levelSettings->m_twoPlayerMode && m_gameState.m_isDualMode) {
            PlayerObject* player = player2 ? m_player2 : m_player1;
            (player->*performButton)(button);
        } else {
            (m_player1->*performButton)(button);
            if (m_gameState.m_isDualMode)
                (m_player2->*performButton)(button);
        }

        m_effectManager->playerButton(down, !player2);

        if (down) {
            m_clicks++;
            if (button == PlayerButton::Jump)
                m_jumping = true;
        }
    }

    // ── processBot: poll and inject inputs at the correct frame ───────────────
    void processBot() {
        auto& bot = Bot::get();
        if (!bot.isPlaying()) return;

        int frame = getProgress(this);

        std::optional<BotInput> input;
        while ((input = bot.poll(frame)) != std::nullopt) {
            simulateClick(
                static_cast<PlayerButton>(input->button),
                input->down,
                input->player2
            );
        }
    }

    // ── processQueuedButtons: inject bot inputs after GD processes its queue ──
    // This matches the original Bot.cpp hook point exactly
    void processQueuedButtons(float dt, bool clearInputQueue) {
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        this->processBot();
    }

    // ── handleButton: record inputs + ignore inputs during playback ───────────
    void handleButton(bool down, int button, bool player1) {
        auto& bot = Bot::get();

        // Ignore real inputs during playback if setting enabled
        if (bot.isPlaying() && bot.getInputCount() > 0) {
            if (Mod::get()->getSettingValue<bool>("ignore-inputs"))
                return;
        }

        GJBaseGameLayer::handleButton(down, button, player1);

        // Record the input
        if (bot.isRecording()) {
            // player1 in handleButton means it IS player1 (not player2)
            bool isPlayer2 = !player1;
            bot.recordInput(
                getProgress(this),
                button,
                isPlayer2,
                down
            );
        }
    }
};

// ════════════════════════════════════════════════════════════════════════════
//  EditorUI fix — reset progress counter when starting playtest
//  (RobTop doesn't reset it, causing bot to fire at wrong frames in editor)
// ════════════════════════════════════════════════════════════════════════════
class $modify(BotEditorUI, EditorUI) {
    void onPlaytest(CCObject* sender) {
        if (auto* editorLayer = LevelEditorLayer::get()) {
            if (editorLayer->m_playbackMode == PlaybackMode::Not) {
                Bot::get().restart();
                editorLayer->m_gameState.m_currentProgress = 0;
            }
        }
        EditorUI::onPlaytest(sender);
    }
};

// ════════════════════════════════════════════════════════════════════════════
//  Mod entry point
// ════════════════════════════════════════════════════════════════════════════
$on_mod(Loaded) {
    // Ensure replays directory exists
    auto dir = Mod::get()->getSaveDir() / "replays";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec))
        std::filesystem::create_directories(dir, ec);

    log::info("[AccurateBot] Loaded — GD 2.2081 Android 32/64-bit");
}
