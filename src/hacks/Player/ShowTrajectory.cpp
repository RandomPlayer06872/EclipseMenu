#include "trajectory.hpp"

#include <array>

#include "bot/bot.hpp"
#include "bot/updater.hpp"
#include "ccTypes.h"
#include "checkpoint/checkpoint.hpp"
#include "physics/collisions.hpp"
#include "physics/object.hpp"
#include "replay/system.hpp"
#include "settings/settings.hpp"

#ifdef SILICATE_PROTECT
#include "VMProtect/VMProtectSDK.h"
#endif

using namespace geode::prelude;

// constexpr std::array<int, 15> INTERACTABLE_IDS = {747,  748,  35,   67,   84,
//                                                   140,  141,  1022, 1332,
//                                                   1333, 1330, 1704, 1751,
//                                                   3005, 3004};

cocos2d::ccColor4F toCocosColor(float colors[4]) {
    return cocos2d::ccColor4F{colors[0], colors[1], colors[2], colors[3]};
}

cocos2d::ccColor4F toCocosColorNegative(float colors[4]) {
    return cocos2d::ccColor4F{1.0f - colors[0], 1.0f - colors[1],
                              1.0f - colors[2], colors[3]};
}

static cocos2d::CCRect usingWidth(const cocos2d::CCRect& old, const float width) {
    cocos2d::CCRect rect = old;
    rect.origin.x += width;
    rect.origin.y += width;
    rect.size.width -= width * 2.0;
    rect.size.height -= width * 2.0;
    return rect;
}

int Trajectory::getPredictionLength() {
    auto bot = Bot::get();
    auto pl = GJBaseGameLayer::get();
    if (!pl) return 0;

    return m_state->m_length->inner() *
                                std::max(bot->updater().getTps(), 240.0) /
                                pl->m_gameState.m_timeWarp;
}

bool Trajectory::iterate(GJBaseGameLayer* pl, PlayerObject* player, int mode,
                         float* colors, bool&,
                         std::vector<TrajectoryPlayerData>& cached, PredictionConfig config) {
    cocos2d::CCPoint position = player->getPosition();

    pl->m_gameState.m_totalTime += Bot::get()->updater().getPhysicsDt();
    pl->m_gameState.m_unkDouble3 += Bot::get()->updater().getPhysicsDt() /
                                    Bot::get()->updater().getTimeWarp();
    pl->m_gameState.m_currentProgress++;
    // pl->m_gameState.m_unkUint2++;
    pl->m_gameState.m_unkUint5 +=
        (int)roundf(Bot::get()->updater().getTimeWarp() * 1000.0f);

    player->m_totalTime += Bot::get()->updater().getPhysicsDt();

    float playerSpeed = *(float*)(&pl->m_gameState.m_timeModRelated);
    if (playerSpeed != 0.0) {
        pl->m_gameState.m_timeModRelated = 0;
        pl->m_gameState.m_timeModRelated2 = 0;

        player->updateTimeMod(playerSpeed, true);
    }

    // player->resetTouchedRings(false);

    player->m_collisionLogTop->removeAllObjects();
    player->m_collisionLogBottom->removeAllObjects();
    player->m_collisionLogLeft->removeAllObjects();
    player->m_collisionLogRight->removeAllObjects();

    if (((m_deadP1 && (mode & TrajectoryMode::Player1) != 0) ||
         (m_deadP2 && (mode & TrajectoryMode::Player2) != 0))) {
        if (cached.size() > 1) {
            drawHitbox(player);
        }
        return true;
    }

    // pl->m_effectManager->prepareMoveActions(m_delta / 60.0f, false);
    // pl->processMoveActionsStep(m_delta / 60.0f, true);
    // pl->m_effectManager->postMoveActions();

    for (auto& action : m_actions) {
        if (action.m_delay == 0) {
            // geode::log::info("executing action at pseudo-frame {}", pl->m_gameState.m_currentProgress - 1);
            action.m_func();
            action.m_executed = true;
            continue;
        }

        action.m_delay--;
    }

    std::erase_if(m_actions, [](auto& a) { return a.m_executed; });

    float delta = config.m_overridenTPS == 0.0 ? m_delta : (1.0 / config.m_overridenTPS) * 60.0;
    // float delta = m_delta;

    player->m_playEffects = false;
    player->update(delta);

    player->m_unkUnused3 = player->getRotation();
    player->updateRotation(delta);
    player->m_shipRotation = player->getPosition();  // ?

    if (pl->checkCollisions(player, delta, false) == 1) {
        this->hasDied(player);
    }

    phys::checkSpawnObjects(pl, player);
    pl->m_effectManager->postCollisionCheck();

    m_node->drawSegment(position, player->getPosition(),
                        m_state->m_width->inner(),
                        player == m_fakePlayer1 ? toCocosColor(colors)
                                                : toCocosColorNegative(colors));

    // for (int i = 0; i < pl->m_objects->count(); i++) {
    //     GameObject* object = (GameObject*)pl->m_objects->objectAtIndex(i);
    //     auto fillCol = toCocosColor(colors);
    //     fillCol.a = 0.2f;
    //     m_node->drawRect(object->getObjectRect(), fillCol, 0.5f,
    //     toCocosColor(colors));
    // }

    cached.emplace_back(TrajectoryPlayerData{
        player->getPosition(), player->getObjectRect(),
        player->getObjectRect(0.3, 0.3), player->getRotation(),
        player == m_fakePlayer1, (mode & TrajectoryMode::Hold) != 0, 0});

    return false;  // continue iteration
}

TrajectoryPlayerData Trajectory::runPrediction(GJBaseGameLayer* pl, PlayerObject* player,
                               PlayerObject* other, int mode, float* colors,
                               bool both, CacheMap& cached, PredictionConfig config) {
    auto bot = Bot::get();
    bool hasPerformedClick = false;
    bool hasPerformedClickOther = false;

    cached.emplace(mode | TrajectoryMode::Player1,
                   std::vector<TrajectoryPlayerData>());
    const int iterations = m_state->m_length->inner() *
                           std::max(bot->updater().getTps(), 240.0) /
                           pl->m_gameState.m_timeWarp;

    cached[mode | TrajectoryMode::Player1].reserve(iterations);
    if (both && pl->m_gameState.m_isDualMode) {
        cached.emplace(mode | TrajectoryMode::Player2,
                       std::vector<TrajectoryPlayerData>());
        cached[mode | TrajectoryMode::Player2].reserve(iterations);
    }

    int playerMask = (player == m_fakePlayer1) ? TrajectoryMode::Player1
                                               : TrajectoryMode::Player2;
    int platformerMask = pl->m_isPlatformer ? TrajectoryMode::Platformer : 0;

    std::array<PlayerObject*, 2> players = {player, other};
    int count = (both && pl->m_gameState.m_isDualMode) ? 2 : 1;

    for (int i = 0; i < count; i++) {
        PlayerObject* plr = players[i];
        CCPoint position = plr->getPosition();
        if (Bot::get()->isRecording()) {
            switch (mode & Trajectory::CLICK_MASK) {
                case TrajectoryMode::Hold: {
                    plr->pushButton(PlayerButton::Jump);
                    break;
                }
                case TrajectoryMode::Swift: {
                    plr->pushButton(PlayerButton::Jump);
                    plr->releaseButton(PlayerButton::Jump);
                    break;
                }
                case TrajectoryMode::Release: {
                    plr->releaseButton(PlayerButton::Jump);
                    plr->m_jumpBuffered = false;
                    break;
                }
            }

            if (pl->m_levelSettings->m_platformerMode) {
                switch (mode & Trajectory::DIRECTION_MASK) {
                    case TrajectoryMode::Left: {
                        plr->releaseButton(PlayerButton::Right);
                        plr->pushButton(PlayerButton::Left);
                        break;
                    }
                    case TrajectoryMode::Right: {
                        plr->releaseButton(PlayerButton::Left);
                        plr->pushButton(PlayerButton::Right);
                        break;
                    }
                    default: {
                        plr->releaseButton(PlayerButton::Left);
                        plr->releaseButton(PlayerButton::Right);
                        break;
                    }
                }
            }
        }

        float width = m_state->m_width->inner() / pl->m_gameState.m_cameraZoom;
        m_node->drawSegment(
            position, plr->getPosition(), width,
            plr == m_fakePlayer1 ? toCocosColor(colors)
                                 : toCocosColorNegative(colors));
    }

    bool breakIterationP1 = false;
    bool breakIterationP2 = false;

    int trajectoryInputIndex = Bot::get()->replaySystem().getInputIndex();
    const std::vector<slc::Action>& inputs =
        Bot::get()->replaySystem().m_actionAtom.m_actions;

    int predCount = 0;
    for (int i = 0; i < this->getPredictionLength() && i <= config.m_maxLength; i++) {
        if (Bot::get()->isPlaying()) {
            uint64_t frame = Bot::get()->updater().getFrame() + i;

            while (trajectoryInputIndex < (int)inputs.size() &&
                   inputs[trajectoryInputIndex].m_frame <= frame) {
                const auto& input = inputs[trajectoryInputIndex];
                PlayerObject* p =
                    (input.m_player2) ? m_fakePlayer2 : m_fakePlayer1;
                PlayerObject* other =
                    (input.m_player2) ? m_fakePlayer1 : m_fakePlayer2;
                if (input.m_holding) {
                    p->pushButton(static_cast<PlayerButton>(input.m_type));
                    if (both && pl->m_gameState.m_isDualMode) {
                        other->pushButton(
                            static_cast<PlayerButton>(input.m_type));
                    }
                } else {
                    p->releaseButton(static_cast<PlayerButton>(input.m_type));
                    if (both && pl->m_gameState.m_isDualMode) {
                        other->releaseButton(
                            static_cast<PlayerButton>(input.m_type));
                    }
                }

                trajectoryInputIndex++;
                if (trajectoryInputIndex >= (int)inputs.size()) {
                    break;
                }
            }
        }

        if (!breakIterationP1) {
            breakIterationP1 = this->iterate(
                pl, player, mode | playerMask, colors, hasPerformedClick,
                cached[mode | platformerMask | playerMask], config);
            predCount++;
        }
        if (both && pl->m_gameState.m_isDualMode && !breakIterationP2) {
            breakIterationP2 = this->iterate(
                pl, other, mode | TrajectoryMode::Player2, colors,
                hasPerformedClickOther,
                cached[mode | platformerMask | TrajectoryMode::Player2], config);
        }
    }

    return {
        .position = player->getPosition(),
        .hitbox = player->getObjectRect(),
        .innerHitbox = player->getObjectRect(0.3, 0.3),
        .rotation = player->getRotationX(),
        .p1 = (mode & TrajectoryMode::Player1) != 0,
        .holding = (mode & TrajectoryMode::Hold) != 0,
        .score = predCount,
    };
}

TrajectoryPlayerData Trajectory::simulate(GJBaseGameLayer* pl, bool p1, int mode,
                          bool clickBothPlayers, PredictionConfig config) {
    // VMProtectBeginMutation("TrajectorySimulation");

    auto player = p1 ? m_fakePlayer1 : m_fakePlayer2;
    auto realPlayer = p1 ? pl->m_player1 : pl->m_player2;
    auto otherPlayer = p1 ? m_fakePlayer2 : m_fakePlayer1;
    auto otherRealPlayer = p1 ? pl->m_player2 : pl->m_player1;

    bool isLeft = (mode & TrajectoryMode::Left) != 0;
    bool isRight = (mode & TrajectoryMode::Right) != 0;
    bool isDirectionless = !(isLeft || isRight);
    bool holdingDirection = (isLeft && realPlayer->m_holdingLeft) ||
                            (isRight && realPlayer->m_holdingRight) ||
                            (isDirectionless && !realPlayer->m_holdingLeft &&
                             !realPlayer->m_holdingRight);
    bool holdingOpposite = (isLeft && realPlayer->m_holdingRight) ||
                           (isRight && realPlayer->m_holdingLeft);
    int platformerMask = pl->m_isPlatformer ? TrajectoryMode::Platformer : 0;

    if ((!(SLSettings::get()
              ->trajectory.categories[mode | platformerMask]
              .enabled ||
          (SLSettings::get()
               ->trajectory
               .categories[(mode & CLICK_MASK) | TrajectoryMode::FollowPlayer |
                           platformerMask]
               .enabled &&
           holdingDirection) ||
          (SLSettings::get()
               ->trajectory
               .categories[(mode & CLICK_MASK) |
                           TrajectoryMode::FollowOpposite | platformerMask]
               .enabled &&
           holdingOpposite))) && !config.m_bypassConfig) {
        return {};
    }

    GJGameState state = pl->m_gameState;
    EffectManagerState ems;
    pl->m_effectManager->saveToState(ems);
    // std::vector<SavedObjectStateRef> savedObjects;
    // std::vector<SavedActiveObjectState> savedActiveObjects;
    // std::vector<SavedSpecialObjectState> savedSpecialObjects;
    // ((PlayLayer*)pl)->saveDynamicSaveObjects(savedObjects);
    // ((PlayLayer*)pl)
    //     ->saveActiveSaveObjects(savedActiveObjects, savedSpecialObjects);

    player->copyAttributes(realPlayer);
    player->m_maybeReducedEffects = true;
    SavedPlayerCheckpoint checkpoint =
        SavedPlayerCheckpoint::create(realPlayer);
    checkpoint.apply(player);
    // player->setPosition(realPlayer->getPosition());
    // player->setRotation(realPlayer->getRotation());
    player->setPosition(realPlayer->m_position);
    player->setRotation(realPlayer->getRotation());

    if (clickBothPlayers) {
        otherPlayer->copyAttributes(otherRealPlayer);
        otherPlayer->m_maybeReducedEffects = true;
        SavedPlayerCheckpoint checkpoint =
            SavedPlayerCheckpoint::create(otherRealPlayer);
        checkpoint.apply(otherPlayer);

        otherPlayer->setPosition(otherRealPlayer->getPosition());
        otherPlayer->setRotation(otherRealPlayer->getRotation());
    }

    m_deadP1 = false;
    m_deadP2 = false;

    float* colors = SLSettings::get()
                        ->trajectory.categories[mode | platformerMask]
                        .colors.data();

    auto predicted = this->runPrediction(pl, player, otherPlayer, mode, colors, clickBothPlayers,
                        m_data, config);

    player->setVisible(false);
    otherPlayer->setVisible(false);

    deactivateAllRemembered();
    // for (int i = 0; i < pl->m_objects->count(); i++) {
    //     GameObject* object = (GameObject*)pl->m_objects->objectAtIndex(i);

    //     CCPoint pos = object->getRealPosition();

    //     object->resetObject();
    //     // if (object->m_outerSectionIndex >= 0) {
    //     //     if (object->m_positionX > 0.0) {

    //     //     }
    //     // }

    //     object->m_isDirty = true;
    //     object->setObjectRectDirty(true);
    //     object->setOrientedRectDirty(true);
    // }

    pl->m_gameState = state;
    pl->m_effectManager->loadFromState(ems);

    // reinterpret_cast<void(*)(GJBaseGameLayer*)>(reinterpret_cast<void*>(geode::base::get()
    // + 0x398310))(pl);
    // for (auto& obj : savedObjects) {
    //     GameObject* o = obj.m_gameObject;
    //     o->resetObject();
    //     o->m_positionX = obj.m_positionX;
    //     o->m_positionY = obj.m_positionY;
    //     o->m_rotationXOffset = obj.m_rotationXOffset;
    //     o->m_rotationYOffset = obj.m_rotationYOffset;

    //     float addToCustomScaleX = obj.m_addToCustomScaleX;
    //     o->m_scaleXOffset = o->m_scaleXOffset + addToCustomScaleX;
    //     o->m_scaleX = o->m_scaleX + addToCustomScaleX;

    //     o->m_isDirty = true;
    //     o->m_isObjectRectDirty = true;

    //     float addToCustomScaleY = obj.m_addToCustomScaleY;
    //     o->m_scaleYOffset = o->m_scaleYOffset + addToCustomScaleY;
    //     o->m_scaleY = o->m_scaleY + addToCustomScaleY;

    //     o->m_unk4C4 = -1;
    //     o->m_unk4CC = -1;

    //     auto pos = o->getRealPosition();
    //     o->setPosition(pos);
    //     pos = o->getRealPosition();
    //     o->m_lastPosition = pos;
    //     o->setObjectRectDirty(true);
    //     o->setOrientedRectDirty(true);
    //     pl->updateObjectSection(o);
    // }

    // for (auto& obj : savedActiveObjects) {
    //     EffectGameObject* o = (EffectGameObject*)obj.m_gameObject;
    //     o->resetObject();
    //     o->m_activatedByPlayer1 = obj.m_activatedByPlayer1;
    //     o->m_activatedByPlayer2 = obj.m_activatedByPlayer2;
    // }

    // VMProtectEnd();
    return predicted;
}

inline void drawRect(CCDrawNode* node, CCRect& rect, ccColor4F color,
                     float width) {
    // std::array<CCPoint, 4> vertices = {CCPoint{rect.getMinX(),
    // rect.getMinY()},
    //                                    CCPoint{rect.getMaxX(),
    //                                    rect.getMinY()},
    //                                    CCPoint{rect.getMaxX(),
    //                                    rect.getMaxY()},
    //                                    CCPoint{rect.getMinX(),
    //                                    rect.getMaxY()}};

    // node->drawPolygon(vertices.data(), vertices.size(), {0.0, 0.0, 0.0, 0.0},
    //                   width, color);

    node->drawRect(rect, {0.0, 0.0, 0.0, 0.0}, width, color);
}

static void drawRotatedRect(CCDrawNode* node, CCRect& rect, float angle,
                            ccColor4F color, float width) {
    std::array<CCPoint, 4> vertices = {CCPoint{rect.getMinX(), rect.getMinY()},
                                       CCPoint{rect.getMaxX(), rect.getMinY()},
                                       CCPoint{rect.getMaxX(), rect.getMaxY()},
                                       CCPoint{rect.getMinX(), rect.getMaxY()}};

    for (auto& vertex : vertices) {
        vertex = vertex.rotateByAngle({rect.getMidX(), rect.getMidY()},
                                      -CC_DEGREES_TO_RADIANS(angle));
    }

    node->drawPolygon(vertices.data(), vertices.size(), {0.0, 0.0, 0.0, 0.0},
                      width, color);
}

void Trajectory::update(GJBaseGameLayer* pl) {
    if (!pl) return;

    this->m_fakePlayer1->setVisible(false);
    this->m_fakePlayer2->setVisible(false);

    if (auto lel = LevelEditorLayer::get(); lel && lel->m_playbackMode == PlaybackMode::Not) {
        return;
    }

    if (!m_state->m_enabled->inner()) {
        m_node->setVisible(false);
        return;
    }

    m_drawing = true;
    m_node->setVisible(true);

    // m_node->setZOrder(999);
    m_node->clear();

    auto bot = Bot::get();
    m_lastFrame = bot->updater().getFrame();
    m_data.clear();
    m_delta = bot->updater().getPhysicsDt() * 60.0f;

    m_fakePlayer1->setVisible(false);
    if (pl->m_player2) {
        m_fakePlayer2->setVisible(false);
    }

    if (pl->m_player1) {
        simulate(pl, true, TrajectoryMode::Hold,
                 !pl->m_levelSettings->m_twoPlayerMode);
        simulate(pl, true, TrajectoryMode::Swift,
                 !pl->m_levelSettings->m_twoPlayerMode);
        simulate(pl, true, TrajectoryMode::Release,
                 !pl->m_levelSettings->m_twoPlayerMode);

        if (pl->m_isPlatformer) {
            simulate(pl, true, TrajectoryMode::Hold | TrajectoryMode::Left,
                     !pl->m_levelSettings->m_twoPlayerMode);
            simulate(pl, true, TrajectoryMode::Swift | TrajectoryMode::Left,
                     !pl->m_levelSettings->m_twoPlayerMode);
            simulate(pl, true, TrajectoryMode::Release | TrajectoryMode::Left,
                     !pl->m_levelSettings->m_twoPlayerMode);
            simulate(pl, true, TrajectoryMode::Hold | TrajectoryMode::Right,
                     !pl->m_levelSettings->m_twoPlayerMode);
            simulate(pl, true, TrajectoryMode::Swift | TrajectoryMode::Right,
                     !pl->m_levelSettings->m_twoPlayerMode);
            simulate(pl, true, TrajectoryMode::Release | TrajectoryMode::Right,
                     !pl->m_levelSettings->m_twoPlayerMode);
        }

        m_fakePlayer1->setVisible(false);
        if (pl->m_player2) {
            m_fakePlayer2->setVisible(false);
        }
    }
    if (pl->m_player2 && pl->m_gameState.m_isDualMode &&
        pl->m_levelSettings->m_twoPlayerMode) {
        simulate(pl, false, TrajectoryMode::Hold, false);
        simulate(pl, false, TrajectoryMode::Swift, false);
        simulate(pl, false, TrajectoryMode::Release, false);

        if (pl->m_isPlatformer) {
            simulate(pl, false, TrajectoryMode::Hold | TrajectoryMode::Left,
                     false);
            simulate(pl, false, TrajectoryMode::Swift | TrajectoryMode::Left,
                     false);
            simulate(pl, false, TrajectoryMode::Release | TrajectoryMode::Left,
                     false);
            simulate(pl, false, TrajectoryMode::Hold | TrajectoryMode::Right,
                     false);
            simulate(pl, false, TrajectoryMode::Swift | TrajectoryMode::Right,
                     false);
            simulate(pl, false, TrajectoryMode::Release | TrajectoryMode::Right,
                     false);
        }
    }

    m_calculated = true;
    m_drawing = false;

    auto& updater = Bot::get()->updater();
    if (updater.m_layoutMode->inner() && !LevelEditorLayer::get()) {
        constexpr std::array<int, 5> colors = {1000, 1001, 1009, 1013, 1014};
        for (const int color : colors) {
            if (ColorAction* action = pl->m_effectManager->getColorAction(color)) {
                switch (color) {
                    case 1000: {
                        auto& col = SLSettings::get()->layoutBgColor;
                        action->m_color = {
                            static_cast<GLubyte>(col[0] * 255),
                            static_cast<GLubyte>(col[1] * 255),
                            static_cast<GLubyte>(col[2] * 255),
                        };
                        break;
                    }
                    case 1001:
                    case 1009: {
                        auto& col = SLSettings::get()->layoutGroundColor;
                        action->m_color = {
                            static_cast<GLubyte>(col[0] * 255),
                            static_cast<GLubyte>(col[1] * 255),
                            static_cast<GLubyte>(col[2] * 255),
                        };
                        break;
                    }
                    default: {
                        action->m_color = {40, 125, 255};
                    }
                }
            }
        }
    }
}

void Trajectory::drawHitbox(PlayerObject* player) {
    #define CC_COLOR(color_type) *reinterpret_cast<cocos2d::ccColor4F*>(settings.categories[color_type].colors.data())
    float width = m_state->m_width->inner() / GJBaseGameLayer::get()->m_gameState.m_cameraZoom;
    CCRect rect = usingWidth(player->getObjectRect(), width);
    CCRect scaled = usingWidth(player->getObjectRect(0.3, 0.3), width);

    auto& settings = SLSettings::get()->hitboxes;
    using Type = SLSettings::HitboxSettings::Type;

    drawRotatedRect(m_node, rect, player->getRotation(), CC_COLOR(Type::PlayerRotated), width);
    drawRect(m_node, rect, CC_COLOR(Type::Player), width);
    drawRect(m_node, scaled, CC_COLOR(Type::PlayerInner), width);
    #undef CC_COLOR
}

bool Trajectory::playerHasActivated(PlayerObject* player,
                                    EnhancedGameObject* object) {
    if (!object) {
        return false;
    }

    if (object->m_isMultiActivate) {
        return false;
    }

    if (player == m_fakePlayer1) {
        return m_activatedObjectsP1[(uintptr_t)object];
    } else if (player == m_fakePlayer2) {
        return m_activatedObjectsP2[(uintptr_t)object];
    }

    return false;
}

bool Trajectory::realPlayerHasActivated(PlayerObject* player,
                                        EnhancedGameObject* object) {
    if (!object) {
        return false;
    }

    auto pl = GJBaseGameLayer::get();
    if (!isFakePlayer(player)) {
        return phys::hasBeenActivatedByPlayer(player, object);
    }

    PlayerObject* realPlayer =
        player == m_fakePlayer1 ? pl->m_player1 : pl->m_player2;

    return phys::hasBeenActivatedByPlayer(realPlayer, object);
}

static void* RingObject_spawnCircle_orig = nullptr;

void RingObject_spawnCircle(RingObject* self) {
    auto func =
        reinterpret_cast<void (*)(RingObject*)>(RingObject_spawnCircle_orig);

    auto bot = Bot::get();

    if (!bot->trajectory().drawing()) {
        func(self);
    }
}

// #ifdef GEODE_IS_WINDOWS
// $execute {
//     RingObject_spawnCircle_orig =
//         reinterpret_cast<void*>(geode::base::get() + 0x4896d0);

//     (void)Mod::get()->hook(RingObject_spawnCircle_orig, &RingObject_spawnCircle,
//                            "RingObject::spawnCircle",
//                            tulip::hook::TulipConvention::Default);
// }
// #endif
