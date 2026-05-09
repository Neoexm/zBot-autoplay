#ifndef _autobot_planner_hpp
#define _autobot_planner_hpp

#include "AutoBotDebug.hpp"
#include "AutoBotModel.hpp"
#include "../zBot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace geode::prelude;

namespace autobot {

class StateReader {
public:
    void reset() {
        m_hasObservedMotion = false;
        m_lastObservedFrame = -1;
        m_captureTickCounter = 0;
        m_lastCaptureTick = -1;
        m_lastObservedX = 0.f;
        m_lastObservedXSpeed = 0.f;
    }

    PlayerSnapshot capture(GJBaseGameLayer* gl, PlayerObject* player, bool inputHeld, float dt) {
        PlayerSnapshot snapshot;
        int currentCaptureTick = m_captureTickCounter++;
        snapshot.frame = gl ? gl->m_gameState.m_currentProgress : -1;
        snapshot.x = player->getPositionX();
        snapshot.y = player->getPositionY();
        snapshot.deltaTime = std::max(dt, 0.f);
        snapshot.jumpUpdateDt = snapshot.deltaTime * kJumpPositionScale;
        snapshot.yVelocity = static_cast<float>(player->m_yVelocity);
        snapshot.playerSpeed = player->m_playerSpeed;
        snapshot.vehicleSize = player->m_vehicleSize;
        snapshot.speedMultiplier = player->m_speedMultiplier;
        snapshot.onGround = player->m_isOnGround;
        snapshot.gravFlip = player->m_isUpsideDown;
        snapshot.isMini = player->m_vehicleSize < 0.8f || player->getScale() < 0.8f;
        snapshot.isDead = player->m_isDead;
        snapshot.inputHeld = inputHeld;
        snapshot.touchedRing = player->m_touchedRing || player->m_touchedCustomRing || player->m_hasEverHitRing;
        snapshot.touchedPad = player->m_touchedPad;
        snapshot.jumpBuffered = player->m_jumpBuffered || player->m_stateRingJump || player->m_stateRingJump2;
        snapshot.onSlope = player->m_isOnSlope || player->m_wasOnSlope || player->m_maybeUpsideDownSlope;
        snapshot.slopeAngle = player->m_slopeAngle;
        snapshot.dualMode = gl ? gl->m_gameState.m_isDualMode : false;
        snapshot.twoPlayerMode = gl && gl->m_levelSettings ? gl->m_levelSettings->m_twoPlayerMode : false;
        snapshot.mode = detectMode(player);

        float gravity = static_cast<float>(player->m_gravity);
        float gravityMod = player->m_gravityMod;
        if (!std::isfinite(gravity) || std::abs(gravity) < 0.05f) {
            gravity = kDefaultCubeGravity;
            snapshot.gravityApproximate = true;
        }
        if (!std::isfinite(gravityMod) || std::abs(gravityMod) < 0.001f) {
            gravityMod = 1.f;
        }
        gravity = std::abs(gravity * gravityMod);
        if (!std::isfinite(gravity) || gravity < 0.05f) {
            gravity = kDefaultCubeGravity;
            snapshot.gravityApproximate = true;
        }
        snapshot.gravityMagnitude = gravity;
        snapshot.gravityPerTick = gravity;

        snapshot.speedRatio = mappedSpeedRatioFromPlayerSpeed(snapshot.playerSpeed);
        if (!std::isfinite(snapshot.speedRatio) || snapshot.speedRatio < 0.001f) {
            snapshot.speedRatio = kNormalSpeedRatio;
        }

        snapshot.currentXVelocity = static_cast<float>(player->getCurrentXVelocity());
        float fallbackSpeed = 0.f;
        if (std::isfinite(snapshot.currentXVelocity) && snapshot.currentXVelocity > 0.01f && snapshot.deltaTime > 0.f) {
            fallbackSpeed = snapshot.currentXVelocity * snapshot.deltaTime;
            snapshot.speedApproximate = false;
        }
        else if (std::isfinite(m_lastObservedXSpeed) && m_lastObservedXSpeed > 0.01f) {
            fallbackSpeed = m_lastObservedXSpeed;
        }
        else {
            fallbackSpeed = snapshot.speedRatio * kNormalSpeedUnits;
            snapshot.speedApproximate = true;
        }

        snapshot.observedXSpeed = fallbackSpeed;
        snapshot.xSpeedUsedBySimulator = fallbackSpeed;
        snapshot.observedXSpeedPerProgressFrame = fallbackSpeed;
        snapshot.observedXSpeedPerSimTick = fallbackSpeed;
        snapshot.baseXSpeed = snapshot.speedRatio > 0.001f
            ? fallbackSpeed / snapshot.speedRatio
            : fallbackSpeed;

        if (m_hasObservedMotion) {
            snapshot.rawXDelta = snapshot.x - m_lastObservedX;
            snapshot.progressFrameDelta = std::max(0, snapshot.frame - m_lastObservedFrame);
            snapshot.captureTickDelta = std::max(1, currentCaptureTick - m_lastCaptureTick);

            if (snapshot.frame > m_lastObservedFrame) {
                int frameDelta = std::max(1, snapshot.frame - m_lastObservedFrame);
                float observedXSpeedPerProgressFrame = snapshot.rawXDelta / static_cast<float>(frameDelta);
                if (std::isfinite(observedXSpeedPerProgressFrame) && observedXSpeedPerProgressFrame > 0.01f) {
                    snapshot.observedXSpeedPerProgressFrame = observedXSpeedPerProgressFrame;
                }
            }

            if (std::isfinite(snapshot.currentXVelocity) && std::abs(snapshot.currentXVelocity) > 0.01f && std::abs(snapshot.rawXDelta) > 0.0001f) {
                float estimatedDelta = std::abs(snapshot.rawXDelta) / std::abs(snapshot.currentXVelocity);
                if (std::isfinite(estimatedDelta) && estimatedDelta > 0.001f && estimatedDelta < 0.5f) {
                    snapshot.deltaTime = estimatedDelta;
                    snapshot.jumpUpdateDt = estimatedDelta * kJumpPositionScale;
                }
            }

            float observedXSpeedPerSimTick = snapshot.rawXDelta / static_cast<float>(snapshot.captureTickDelta);
            if (std::isfinite(observedXSpeedPerSimTick) && observedXSpeedPerSimTick > 0.01f) {
                snapshot.observedXSpeedPerSimTick = observedXSpeedPerSimTick;
                snapshot.observedXSpeed = observedXSpeedPerSimTick;
                snapshot.xSpeedUsedBySimulator = observedXSpeedPerSimTick;
                snapshot.baseXSpeed = snapshot.speedRatio > 0.001f
                    ? observedXSpeedPerSimTick / snapshot.speedRatio
                    : observedXSpeedPerSimTick;
                snapshot.xSpeedUnitMode = XSpeedUnitMode::SimTick;
                snapshot.speedApproximate = false;
            }
            else if (std::isfinite(m_lastObservedXSpeed) && m_lastObservedXSpeed > 0.01f) {
                snapshot.observedXSpeed = m_lastObservedXSpeed;
                snapshot.xSpeedUsedBySimulator = m_lastObservedXSpeed;
                snapshot.baseXSpeed = snapshot.speedRatio > 0.001f
                    ? m_lastObservedXSpeed / snapshot.speedRatio
                    : m_lastObservedXSpeed;
                snapshot.xSpeedUnitMode = XSpeedUnitMode::SimTick;
            }
            else if (std::isfinite(snapshot.observedXSpeedPerProgressFrame) && snapshot.observedXSpeedPerProgressFrame > 0.01f) {
                snapshot.observedXSpeed = snapshot.observedXSpeedPerProgressFrame;
                snapshot.xSpeedUsedBySimulator = snapshot.observedXSpeedPerProgressFrame;
                snapshot.baseXSpeed = snapshot.speedRatio > 0.001f
                    ? snapshot.observedXSpeedPerProgressFrame / snapshot.speedRatio
                    : snapshot.observedXSpeedPerProgressFrame;
                snapshot.xSpeedUnitMode = XSpeedUnitMode::ProgressFrame;
                snapshot.speedApproximate = false;
            }
        }
        else {
            snapshot.progressFrameDelta = 0;
            snapshot.captureTickDelta = 0;
            snapshot.rawXDelta = 0.f;
        }

        m_hasObservedMotion = true;
        m_lastObservedFrame = snapshot.frame;
        m_lastCaptureTick = currentCaptureTick;
        m_lastObservedX = snapshot.x;
        m_lastObservedXSpeed = snapshot.xSpeedUsedBySimulator;
        return snapshot;
    }

private:
    bool m_hasObservedMotion = false;
    int m_lastObservedFrame = -1;
    int m_captureTickCounter = 0;
    int m_lastCaptureTick = -1;
    float m_lastObservedX = 0.f;
    float m_lastObservedXSpeed = 0.f;

    static GameMode detectMode(PlayerObject* player) {
        if (player->m_isShip)   return GameMode::Ship;
        if (player->m_isBall)   return GameMode::Ball;
        if (player->m_isBird)   return GameMode::UFO;
        if (player->m_isDart)   return GameMode::Wave;
        if (player->m_isRobot)  return GameMode::Robot;
        if (player->m_isSpider) return GameMode::Spider;
        if (player->m_isSwing)  return GameMode::Swing;
        return GameMode::Cube;
    }

    static float mappedSpeedRatioFromPlayerSpeed(float playerSpeed) {
        if (playerSpeed <= 0.7f) return kHalfSpeedRatio;
        if (playerSpeed <= 1.1f) return kNormalSpeedRatio;
        if (playerSpeed <= 1.3f) return kDoubleSpeedRatio;
        if (playerSpeed <= 1.6f) return kTripleSpeedRatio;
        return kQuadSpeedRatio;
    }
};

class LevelObjectCache {
public:
    void invalidate() {
        m_objects.clear();
        m_loggedUnknownIDs.clear();
        m_lastObjectCount = 0;
        m_built = false;
    }

    bool isBuilt() const {
        return m_built;
    }

    size_t size() const {
        return m_objects.size();
    }

    bool build(GJBaseGameLayer* gl, bool logUnknownObjects) {
        invalidate();

        if (!gl || !gl->m_objects) {
            return false;
        }

        auto total = gl->m_objects->count();
        m_lastObjectCount = total;
        m_objects.reserve(total);

        for (unsigned int i = 0; i < total; ++i) {
            auto* obj = static_cast<GameObject*>(gl->m_objects->objectAtIndex(i));
            if (!obj) continue;
            if (!isReadableObjectPtr(obj)) continue;
            if (geode::DestructorLock::isLocked(static_cast<cocos2d::CCNode*>(obj))) continue;

            auto cached = buildObject(obj);
            if (!cached) {
                if (logUnknownObjects) {
                    logUnknownObject(obj, "level-cache");
                }
                continue;
            }

            m_objects.push_back(*cached);
        }

        std::sort(m_objects.begin(), m_objects.end(), [](CachedLevelObject const& a, CachedLevelObject const& b) {
            if (a.bounds.left != b.bounds.left) return a.bounds.left < b.bounds.left;
            if (a.bounds.bottom != b.bounds.bottom) return a.bounds.bottom < b.bounds.bottom;
            return a.objectID < b.objectID;
        });

        m_built = true;
        return true;
    }

    void queryRange(float left, float right, std::vector<CachedLevelObject const*>& out) const {
        out.clear();
        if (m_objects.empty()) return;

        auto begin = std::lower_bound(
            m_objects.begin(),
            m_objects.end(),
            left,
            [](CachedLevelObject const& object, float value) {
                return object.bounds.right < value;
            }
        );

        for (auto it = begin; it != m_objects.end() && it->bounds.left <= right; ++it) {
            out.push_back(&*it);
        }
    }

    void queryCategorizedRange(float left, float right, NearbyObjectSet& out) const {
        out.clear();
        if (m_objects.empty()) return;

        auto begin = std::lower_bound(
            m_objects.begin(),
            m_objects.end(),
            left,
            [](CachedLevelObject const& object, float value) {
                return object.bounds.right < value;
            }
        );

        for (auto it = begin; it != m_objects.end() && it->bounds.left <= right; ++it) {
            auto const* object = &*it;
            out.all.push_back(object);

            switch (object->category) {
                case CachedObjectCategory::Solid:
                    out.solids.push_back(object);
                    break;
                case CachedObjectCategory::Hazard:
                    out.hazards.push_back(object);
                    break;
                case CachedObjectCategory::Orb:
                    out.orbs.push_back(object);
                    out.interactives.push_back(object);
                    break;
                case CachedObjectCategory::Pad:
                    out.pads.push_back(object);
                    out.interactives.push_back(object);
                    break;
                case CachedObjectCategory::Portal:
                    out.portals.push_back(object);
                    out.interactives.push_back(object);
                    break;
                default:
                    break;
            }
        }
    }

private:
    std::vector<CachedLevelObject> m_objects;
    std::unordered_set<int> m_loggedUnknownIDs;
    unsigned int m_lastObjectCount = 0;
    bool m_built = false;

    static bool isSolidObject(GameObject* obj) {
        if (!obj) return false;
        if (obj->m_isSolidColorBlock) return true;

        switch (obj->m_objectType) {
            case GameObjectType::Solid:
            case GameObjectType::Slope:
            case GameObjectType::Breakable:
                return true;
            default:
                return false;
        }
    }

    static bool isReadableObjectPtr(GameObject* obj) {
        if (!obj) {
            return false;
        }

#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mbi{};
        if (!::VirtualQuery(obj, &mbi, sizeof(mbi))) {
            return false;
        }

        if (mbi.State != MEM_COMMIT) {
            return false;
        }

        if ((mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) {
            return false;
        }

        auto begin = reinterpret_cast<std::uintptr_t>(obj);
        auto end = begin + sizeof(GameObject);
        auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (end > regionEnd) {
            return false;
        }
#endif

        return true;
    }

    static bool isReasonableRect(RectF const& rect) {
        return std::isfinite(rect.left)
            && std::isfinite(rect.right)
            && std::isfinite(rect.bottom)
            && std::isfinite(rect.top)
            && rect.width() > 0.5f
            && rect.height() > 0.5f
            && rect.width() < 4096.f
            && rect.height() < 4096.f
            && std::abs(rect.left) < 10000000.f
            && std::abs(rect.right) < 10000000.f
            && std::abs(rect.bottom) < 10000000.f
            && std::abs(rect.top) < 10000000.f;
    }

    static std::pair<RectF, CollisionApproximation> safeObjectRect(GameObject* obj) {
        cocos2d::CCRect rect = obj->m_objectRect;
        RectF runtimeRect {
            rect.getMinX(),
            rect.getMaxX(),
            rect.getMinY(),
            rect.getMaxY(),
        };

        bool validRuntimeRect = std::isfinite(runtimeRect.left)
            && std::isfinite(runtimeRect.right)
            && std::isfinite(runtimeRect.bottom)
            && std::isfinite(runtimeRect.top)
            && runtimeRect.right > runtimeRect.left
            && runtimeRect.top > runtimeRect.bottom
            && !obj->m_isObjectRectDirty;

        if (validRuntimeRect) {
            return { runtimeRect, CollisionApproximation::GameRect };
        }

        float scaleX = std::abs(obj->m_scaleX);
        float scaleY = std::abs(obj->m_scaleY);
        if (scaleX < 0.001f) scaleX = 1.f;
        if (scaleY < 0.001f) scaleY = 1.f;

        float width = std::abs(obj->m_width) * scaleX;
        float height = std::abs(obj->m_height) * scaleY;
        float radius = std::max(0.f, obj->m_objectRadius);
        if (width < 4.f) width = radius > 0.f ? radius * 2.f : 30.f;
        if (height < 4.f) height = radius > 0.f ? radius * 2.f : 30.f;

        float boxOffsetX = obj->m_boxOffsetCalculated ? obj->m_boxOffset.x : 0.f;
        float boxOffsetY = obj->m_boxOffsetCalculated ? obj->m_boxOffset.y : 0.f;
        float centerX = static_cast<float>(obj->m_positionX) + boxOffsetX + obj->m_customBoxOffset.x;
        float centerY = static_cast<float>(obj->m_positionY) + boxOffsetY + obj->m_customBoxOffset.y;

        RectF fallbackRect {
            centerX - width * 0.5f,
            centerX + width * 0.5f,
            centerY - height * 0.5f,
            centerY + height * 0.5f,
        };

        return { fallbackRect, CollisionApproximation::FallbackAABB };
    }

    std::optional<CachedLevelObject> buildObject(GameObject* obj) const {
        CachedLevelObject cached;
        cached.uniqueID = obj->m_uniqueID;
        cached.objectID = obj->m_objectID;
        cached.objectType = obj->m_objectType;
        cached.orbKind = classifyOrbID(obj->m_objectID);
        cached.padKind = classifyPadID(obj->m_objectID);
        cached.portalKind = classifyPortalID(obj->m_objectID);
        cached.targetMode = gameModeFromPortalKind(cached.portalKind);
        cached.speedValue = speedUnitsFromPortalKind(cached.portalKind);
        cached.rotation = obj->getRotation();
        cached.scaleX = obj->m_scaleX;
        cached.scaleY = obj->m_scaleY;
        cached.flipX = obj->m_isFlipX;
        cached.flipY = obj->m_isFlipY;
        cached.solidColorBlock = obj->m_isSolidColorBlock;
        cached.slopeIsHazard = obj->m_slopeIsHazard;
        cached.isGripSlope = obj->m_isGripSlope;
        cached.slopeDirection = obj->m_slopeDirection;

        if (obj->m_isDecoration || obj->m_isDecoration2) {
            return std::nullopt;
        }

        auto [bounds, approximation] = safeObjectRect(obj);
        if (!isReasonableRect(bounds)) {
            return std::nullopt;
        }

        cached.bounds = bounds;
        cached.collisionApproximation = approximation;
        cached.runtimeRect = approximation == CollisionApproximation::GameRect;
        if (std::abs(cached.rotation) > 0.01f || obj->m_objectType == GameObjectType::Slope || obj->m_slopeIsHazard) {
            cached.collisionApproximation = CollisionApproximation::FallbackAABB;
        }

        if (obj->m_objectType == GameObjectType::Hazard || isHazardID(obj->m_objectID) || obj->m_slopeIsHazard) {
            cached.category = CachedObjectCategory::Hazard;
        }
        else if (cached.orbKind != OrbKind::None) {
            cached.category = CachedObjectCategory::Orb;
        }
        else if (cached.padKind != PadKind::None) {
            cached.category = CachedObjectCategory::Pad;
        }
        else if (cached.portalKind != PortalKind::None) {
            cached.category = CachedObjectCategory::Portal;
        }
        else if (isSolidObject(obj)) {
            cached.category = CachedObjectCategory::Solid;
        }
        else {
            return std::nullopt;
        }

        return cached;
    }

    void logUnknownObject(GameObject* obj, char const* source) {
        if (!obj) return;
        int id = obj->m_objectID;
        if (!m_loggedUnknownIDs.insert(id).second) return;

        std::ostringstream line;
        line << "source=" << source
             << " id=" << id
             << " type=" << static_cast<int>(obj->m_objectType)
             << " x=" << obj->m_positionX
             << " y=" << obj->m_positionY;
        log::warn("[AutoBot] unknown gameplay object encountered | {}", line.str());
        AutoBotDebug::get()->logEvent("unknown-object", line.str());
    }
};

class CollisionProvider {
public:
    static auto rangeBegin(std::vector<CachedLevelObject const*> const& objects, float left) {
        return std::lower_bound(
            objects.begin(),
            objects.end(),
            left,
            [](CachedLevelObject const* object, float value) {
                return object->bounds.right < value;
            }
        );
    }

    static RectF playerBounds(SimState const& state) {
        return {
            state.x - state.halfWidth(),
            state.x + state.halfWidth(),
            state.y - state.halfHeight(),
            state.y + state.halfHeight(),
        };
    }

    static float inferGroundY(SimState const& state, std::vector<CachedLevelObject const*> const& objects) {
        float footY = state.y - state.halfHeight();
        float probeLeft = state.x - state.halfWidth() - 6.f;
        float probeRight = state.x + state.halfWidth() + 6.f;
        float bestTop = -std::numeric_limits<float>::infinity();
        bool found = false;

        for (auto const* object : objects) {
            if (!object || !object->isSolid()) continue;
            if (object->bounds.right < probeLeft || object->bounds.left > probeRight) continue;
            if (object->bounds.top <= footY + 18.f && object->bounds.top >= footY - 180.f) {
                bestTop = std::max(bestTop, object->bounds.top);
                found = true;
            }
        }

        if (state.hasDefaultGroundSupport && !state.gravFlip) {
            bestTop = std::max(bestTop, state.defaultGroundY);
            found = true;
        }

        if (!found) {
            return state.y - 600.f;
        }

        return bestTop + state.halfHeight();
    }

    static float inferCeilingY(SimState const& state, std::vector<CachedLevelObject const*> const& objects) {
        float headY = state.y + state.halfHeight();
        float probeLeft = state.x - state.halfWidth() - 6.f;
        float probeRight = state.x + state.halfWidth() + 6.f;
        float bestBottom = std::numeric_limits<float>::infinity();
        bool found = false;

        for (auto const* object : objects) {
            if (!object || !object->isSolid()) continue;
            if (object->bounds.right < probeLeft || object->bounds.left > probeRight) continue;
            if (object->bounds.bottom >= headY - 18.f && object->bounds.bottom <= headY + 220.f) {
                bestBottom = std::min(bestBottom, object->bounds.bottom);
                found = true;
            }
        }

        if (!found) {
            return state.y + 600.f;
        }

        return bestBottom - state.halfHeight();
    }

    static bool isNearLikelyDefaultGroundBand(float groundY) {
        return std::abs(groundY - kLikelyDefaultGroundY) <= kDefaultGroundBandTolerance;
    }

    static void resolveDefaultGroundSupport(SimState& state, RectF const& previousBounds) {
        if (!state.hasDefaultGroundSupport || state.gravFlip || state.onGround) {
            return;
        }

        RectF currentBounds = playerBounds(state);
        if (previousBounds.bottom >= state.defaultGroundY - 8.f && currentBounds.bottom <= state.defaultGroundY + 2.f) {
            state.y = state.defaultGroundY + state.halfHeight();
            state.yVelocity = 0.f;
            state.onGround = true;
            state.currentSupportUID = kSyntheticDefaultGroundSupportUID;
            state.supportSource = SupportSource::DefaultGround;
        }
    }

    static bool hasSupportAt(float centerX, float bottomY, float halfWidth, std::vector<CachedLevelObject const*> const& objects, float tolerance = 10.f) {
        float probeLeft = centerX - halfWidth;
        float probeRight = centerX + halfWidth;

        for (auto const* object : objects) {
            if (!object || !object->isSolid()) continue;
            if (object->bounds.right < probeLeft || object->bounds.left > probeRight) continue;
            if (std::abs(object->bounds.top - bottomY) <= tolerance) {
                return true;
            }
        }

        return false;
    }

    static bool hasHazardBelow(float centerX, float y, float halfWidth, std::vector<CachedLevelObject const*> const& objects, float maxDrop = 140.f) {
        float probeLeft = centerX - halfWidth - 12.f;
        float probeRight = centerX + halfWidth + 12.f;

        for (auto const* object : objects) {
            if (!object || !object->isHazard()) continue;
            if (object->bounds.right < probeLeft || object->bounds.left > probeRight) continue;

            float drop = y - object->bounds.top;
            if (drop >= -12.f && drop <= maxDrop) {
                return true;
            }
        }

        return false;
    }

    static bool resolveCubeSolids(
        SimState& state,
        RectF const& previousBounds,
        std::vector<CachedLevelObject const*> const& solids,
        std::string& deathReason,
        PlannerDecision* metrics = nullptr
    ) {
        RectF currentBounds = playerBounds(state);
        auto begin = rangeBegin(solids, currentBounds.left - 18.f);

        for (auto it = begin; it != solids.end() && (*it)->bounds.left <= currentBounds.right + 18.f; ++it) {
            auto const* object = *it;
            if (!object || !object->isSolid()) continue;
            if (metrics) {
                ++metrics->solidChecks;
            }
            if (!currentBounds.overlaps(object->bounds)) continue;

            state.worstApproximation = mergeApproximation(state.worstApproximation, object->collisionApproximation);

            bool overlapX = currentBounds.right > object->bounds.left + 1.f && currentBounds.left < object->bounds.right - 1.f;
            bool overlapY = currentBounds.top > object->bounds.bottom + 1.f && currentBounds.bottom < object->bounds.top - 1.f;

            if (!state.gravFlip && overlapX && previousBounds.bottom >= object->bounds.top - 8.f && currentBounds.bottom <= object->bounds.top + 2.f) {
                state.y = object->bounds.top + state.halfHeight();
                state.yVelocity = 0.f;
                state.onGround = true;
                state.currentSupportUID = object->uniqueID;
                state.supportSource = SupportSource::Solid;
                if (state.hasDefaultGroundSupport && std::abs(object->bounds.top - state.defaultGroundY) > 4.f) {
                    state.hasDefaultGroundSupport = false;
                }
                currentBounds = playerBounds(state);
                continue;
            }

            if (state.gravFlip && overlapX && previousBounds.top <= object->bounds.bottom + 8.f && currentBounds.top >= object->bounds.bottom - 2.f) {
                state.y = object->bounds.bottom - state.halfHeight();
                state.yVelocity = 0.f;
                state.onGround = true;
                state.currentSupportUID = object->uniqueID;
                state.supportSource = SupportSource::Solid;
                state.hasDefaultGroundSupport = false;
                currentBounds = playerBounds(state);
                continue;
            }

            bool hitLeftWall = overlapY && previousBounds.right <= object->bounds.left + 4.f && currentBounds.right >= object->bounds.left;
            bool hitRightWall = overlapY && previousBounds.left >= object->bounds.right - 4.f && currentBounds.left <= object->bounds.right;
            bool hitUnderside = overlapX && !state.gravFlip && previousBounds.top <= object->bounds.bottom + 4.f && currentBounds.top >= object->bounds.bottom;
            bool hitTopside = overlapX && state.gravFlip && previousBounds.bottom >= object->bounds.top - 4.f && currentBounds.bottom <= object->bounds.top;

            if (hitLeftWall || hitRightWall) {
                deathReason = "solid-wall";
                state.isDead = true;
                return false;
            }

            if (hitUnderside || hitTopside) {
                state.y = hitUnderside ? object->bounds.bottom - state.halfHeight() : object->bounds.top + state.halfHeight();
                state.yVelocity = 0.f;
                currentBounds = playerBounds(state);
            }
        }

        return true;
    }

    static int findSupportingSolidUID(SimState const& state, std::vector<CachedLevelObject const*> const& solids, float tolerance = 12.f) {
        float footY = state.y - state.halfHeight();
        float probeLeft = state.x - state.halfWidth();
        float probeRight = state.x + state.halfWidth();
        auto begin = rangeBegin(solids, probeLeft - 4.f);

        for (auto it = begin; it != solids.end() && (*it)->bounds.left <= probeRight + 4.f; ++it) {
            auto const* object = *it;
            if (!object || !object->isSolid()) continue;
            if (object->bounds.right < probeLeft || object->bounds.left > probeRight) continue;
            if (std::abs(object->bounds.top - footY) <= tolerance || std::abs(object->bounds.bottom - (state.y + state.halfHeight())) <= tolerance) {
                return object->uniqueID;
            }
        }

        return -1;
    }

    static bool isLikelySpike(CachedLevelObject const& object) {
        switch (object.objectID) {
            case 8: case 39: case 103: case 135:
            case 392: case 9: case 61: case 243: case 244:
            case 363: case 364: case 365: case 366:
            case 367: case 368: case 369: case 370:
            case 446: case 447: case 667: case 720:
            case 721: case 722:
                return true;
            default:
                return false;
        }
    }

    static void fillHazardCollisionInfo(
        HazardCollisionInfo& info,
        CachedLevelObject const& object,
        RectF const& playerBounds,
        bool usedSpikeApproximation
    ) {
        info.valid = true;
        info.objectID = object.objectID;
        info.uniqueID = object.uniqueID;
        info.category = object.category;
        info.objectBounds = object.bounds;
        info.playerBounds = playerBounds;
        info.likelySpike = isLikelySpike(object);
        info.approximation = usedSpikeApproximation ? CollisionApproximation::FallbackAABB : object.collisionApproximation;
        info.collisionType = usedSpikeApproximation ? "spike-triangle" : "aabb";
    }

    static bool spikeOverlapsApprox(RectF const& playerBounds, CachedLevelObject const& object) {
        float overlapBottom = std::max(playerBounds.bottom, object.bounds.bottom);
        float overlapTop = std::min(playerBounds.top, object.bounds.top);
        if (overlapTop <= overlapBottom) {
            return false;
        }

        float centerX = object.bounds.centerX();
        float height = std::max(object.bounds.height(), 0.001f);
        float halfWidth = object.bounds.width() * 0.5f;
        float sampleYs[3] = {
            overlapBottom + 1.f,
            (overlapBottom + overlapTop) * 0.5f,
            overlapTop - 1.f,
        };

        for (float sampleY : sampleYs) {
            if (sampleY <= object.bounds.bottom || sampleY >= object.bounds.top) continue;
            float rel = std::clamp((sampleY - object.bounds.bottom) / height, 0.f, 1.f);
            float activeHalfWidth = halfWidth * (1.f - rel * 0.92f);
            float lethalLeft = centerX - activeHalfWidth;
            float lethalRight = centerX + activeHalfWidth;
            if (playerBounds.right > lethalLeft && playerBounds.left < lethalRight) {
                return true;
            }
        }

        return false;
    }

    static float rectSeparation(RectF const& a, RectF const& b) {
        float horizontalGap = std::max({
            b.left - a.right,
            a.left - b.right,
            0.f,
        });
        float verticalGap = std::max({
            b.bottom - a.top,
            a.bottom - b.top,
            0.f,
        });

        if (horizontalGap > 0.f && verticalGap > 0.f) {
            return std::sqrt(horizontalGap * horizontalGap + verticalGap * verticalGap);
        }
        if (horizontalGap > 0.f) {
            return horizontalGap;
        }
        if (verticalGap > 0.f) {
            return verticalGap;
        }
        return 0.f;
    }

    static float spikeClearanceApprox(RectF const& playerBounds, CachedLevelObject const& object) {
        float centerX = object.bounds.centerX();
        float height = std::max(object.bounds.height(), 0.001f);
        float halfWidth = object.bounds.width() * 0.5f;
        float bestClearance = std::numeric_limits<float>::infinity();
        float sampleYs[5] = {
            object.bounds.bottom + 1.f,
            object.bounds.bottom + height * 0.25f,
            object.bounds.bottom + height * 0.50f,
            object.bounds.bottom + height * 0.75f,
            object.bounds.top - 1.f,
        };

        for (float sampleY : sampleYs) {
            if (sampleY <= object.bounds.bottom || sampleY >= object.bounds.top) continue;

            float rel = std::clamp((sampleY - object.bounds.bottom) / height, 0.f, 1.f);
            float activeHalfWidth = halfWidth * (1.f - rel * 0.92f);
            RectF lethalBand {
                centerX - activeHalfWidth,
                centerX + activeHalfWidth,
                sampleY - 1.f,
                sampleY + 1.f,
            };
            bestClearance = std::min(bestClearance, rectSeparation(playerBounds, lethalBand));
        }

        if (!std::isfinite(bestClearance)) {
            return rectSeparation(playerBounds, object.bounds);
        }
        return bestClearance;
    }

    static void updateMinHazardClearance(
        RectF const& bounds,
        std::vector<CachedLevelObject const*> const& hazards,
        CubeSimulationResult& result,
        PlannerDecision* metrics = nullptr
    ) {
        auto begin = rangeBegin(hazards, bounds.left - 48.f);
        for (auto it = begin; it != hazards.end() && (*it)->bounds.left <= bounds.right + 48.f; ++it) {
            auto const* object = *it;
            if (!object || !object->isHazard()) continue;
            if (metrics) {
                ++metrics->clearanceChecks;
            }

            float clearance = isLikelySpike(*object)
                ? spikeClearanceApprox(bounds, *object)
                : rectSeparation(bounds, object->bounds);
            if (!std::isfinite(clearance)) continue;

            result.hazardClearanceComputed = true;
            if (isLikelySpike(*object) || object->collisionApproximation != CollisionApproximation::GameRect) {
                result.hazardClearanceApproximate = true;
            }
            result.minHazardClearance = std::min(result.minHazardClearance, clearance);
        }
    }

    static bool touchesHazard(
        SimState& state,
        std::vector<CachedLevelObject const*> const& hazards,
        std::string& deathReason,
        HazardCollisionInfo* collisionInfo = nullptr,
        PlannerDecision* metrics = nullptr
    ) {
        RectF bounds = playerBounds(state);
        auto begin = rangeBegin(hazards, bounds.left - 18.f);

        for (auto it = begin; it != hazards.end() && (*it)->bounds.left <= bounds.right + 18.f; ++it) {
            auto const* object = *it;
            if (!object || !object->isHazard()) continue;
            if (metrics) {
                ++metrics->hazardChecks;
            }
            bool usesSpikeApproximation = isLikelySpike(*object);
            bool overlapsHazard = usesSpikeApproximation
                ? spikeOverlapsApprox(bounds, *object)
                : bounds.overlaps(object->bounds);
            if (!overlapsHazard) continue;

            if (collisionInfo) {
                fillHazardCollisionInfo(*collisionInfo, *object, bounds, usesSpikeApproximation);
            }
            state.worstApproximation = mergeApproximation(
                state.worstApproximation,
                usesSpikeApproximation ? CollisionApproximation::FallbackAABB : object->collisionApproximation
            );
            deathReason = "hazard";
            state.isDead = true;
            return true;
        }

        return false;
    }
};

class RollingLookaheadPlanner {
public:
    static CubeSimulationTrace traceCubeCandidate(
        PlayerSnapshot const& snapshot,
        NearbyObjectSet const& nearby,
        PlannerConfig const& config,
        CubeCandidate const& candidate,
        int horizonTicks,
        bool computeClearance = false
    ) {
        CubeSimulationTrace trace;
        trace.result = simulateCubeCandidate(snapshot, nearby, config, candidate, horizonTicks, computeClearance, nullptr, &trace.samples);
        return trace;
    }

    void reset() {
        m_lastDecision = {};
        m_budgetCooldownTicks = 0;
    }

    PlannerDecision planCube(PlayerSnapshot const& snapshot, NearbyObjectSet const& nearby, PlannerConfig const& config) {
        using Clock = std::chrono::steady_clock;
        auto planStart = Clock::now();
        auto elapsedMs = [&]() {
            return std::chrono::duration<double, std::milli>(Clock::now() - planStart).count();
        };

        PlannerDecision decision;
        decision.valid = true;
        decision.rollingPlanner = true;
        decision.configuredHorizonSeconds = config.horizonSeconds;
        decision.horizonSeconds = kRollingCubePlannerHorizonSeconds;
        decision.leadFrames = snapshot.isMini ? 12.5f : 10.5f;
        decision.configuredSafetyTicks = std::clamp(config.cubeTimingSafetyTicks, 0, kMaxCubeTimingSafetyTicks);
        decision.selectedTapLabel = "no-input";
        decision.noInputRejectedReason = "no-input-safe";
        decision.dropGateRejectedReason = "not-checked";
        decision.nearbyHazards = static_cast<int>(nearby.hazards.size());
        decision.nearbySolids = static_cast<int>(nearby.solids.size());
        decision.nearbyInteractives = static_cast<int>(nearby.interactiveCount());

        bool budgetCooldownActive = m_budgetCooldownTicks > 0;
        if (m_budgetCooldownTicks > 0) {
            --m_budgetCooldownTicks;
        }

        auto finalizeDecision = [&](char const* expensiveReason = nullptr) {
            decision.planTimeMs = elapsedMs();
            if (decision.expensivePlanningReason.empty() && expensiveReason) {
                decision.expensivePlanningReason = expensiveReason;
            }
            if (decision.planTimeMs > kPlannerHardBudgetMs || decision.plannerBudgetExceeded) {
                decision.plannerBudgetExceeded = true;
                decision.usedBudgetFallback = true;
                m_budgetCooldownTicks = std::max(m_budgetCooldownTicks, 30);
            }
            m_lastDecision = decision;
            return decision;
        };

        float playerFront = snapshot.x + (snapshot.isMini ? 7.5f : 15.f);
        float footY = snapshot.y - (snapshot.isMini ? 7.5f : 15.f);
        float checkDist = std::clamp(snapshot.observedXSpeed * 18.0f, 90.0f, 260.0f);
        float checkLeft = playerFront - 4.f;
        float checkRight = playerFront + checkDist;
        int immediateGroundHazardCount = 0;
        CachedLevelObject const* nearestGroundHazard = nullptr;

        for (auto const* object : nearby.hazards) {
            if (!object || !object->isHazard()) continue;
            if (object->bounds.right < checkLeft || object->bounds.left > checkRight) continue;

            float hazardWidth = object->bounds.width();
            float hazardHeight = object->bounds.height();
            if (hazardWidth < 2.f || hazardWidth > 80.f) continue;
            if (hazardHeight < 4.f || hazardHeight > 60.f) continue;

            bool crossesGroundBand = !(object->bounds.top < footY - 10.f || object->bounds.bottom > footY + 40.f);
            bool rootedOnFloor = std::abs(object->bounds.bottom - footY) <= 16.f;
            bool risesIntoPlayer = object->bounds.top >= footY + 12.f;
            if (!crossesGroundBand && !(rootedOnFloor && risesIntoPlayer)) continue;

            float distanceAhead = object->bounds.left - playerFront;
            if (distanceAhead < decision.nearestHazardDistance) {
                decision.nearestHazardDistance = distanceAhead;
                nearestGroundHazard = object;
            }
            if (distanceAhead >= -8.f && distanceAhead <= 96.f) {
                ++immediateGroundHazardCount;
            }
        }

        if (nearestGroundHazard) {
            decision.nearestHazardObjectID = nearestGroundHazard->objectID;
            decision.nearestHazardObjectUID = nearestGroundHazard->uniqueID;
            decision.nearestHazardLeft = nearestGroundHazard->bounds.left;
            decision.nearestHazardRight = nearestGroundHazard->bounds.right;
            decision.nearestHazardBottom = nearestGroundHazard->bounds.bottom;
            decision.nearestHazardTop = nearestGroundHazard->bounds.top;
            decision.nearestHazardLikelySpike = CollisionProvider::isLikelySpike(*nearestGroundHazard);
            decision.nearestHazardApproximation = nearestGroundHazard->collisionApproximation;
        }

        float safeXSpeed = std::max(snapshot.observedXSpeed, 0.001f);
        decision.timeToHazard = std::isfinite(decision.nearestHazardDistance)
            ? decision.nearestHazardDistance / safeXSpeed
            : std::numeric_limits<float>::infinity();

        bool preComplex = snapshot.onSlope
            || decision.nearbyInteractives > 0
            || decision.nearbyHazards > 1
            || decision.nearbySolids > 3;
        if (preComplex) {
            decision.expensivePlanningReason = "complex-nearby-objects";
        }

        int horizonTicks = std::clamp(
            static_cast<int>(std::round(config.ticksPerSecond * decision.horizonSeconds)),
            45,
            preComplex ? kPlannerComplexMaxHorizonTicks : kPlannerNormalMaxHorizonTicks
        );
        decision.horizonTicks = horizonTicks;
        decision.proactivePlanTicks = std::max(kCubeProactivePlanTicks, decision.configuredSafetyTicks + 18);

        auto noJump = simulateCubeCandidate(snapshot, nearby, config, CubeCandidate { {}, false, "no-input" }, horizonTicks, false, &decision);
        decision.candidateCount = 1;
        decision.hasDefaultGroundSupport = noJump.hasDefaultGroundSupport;
        decision.defaultGroundY = noJump.defaultGroundY;
        decision.supportSource = noJump.initialSupportSource;
        decision.currentSupportUID = noJump.initialSupportUID;
        decision.noJumpDeathTick = noJump.deathTick;
        decision.noInputSupportLostTick = noJump.supportLostTick;
        decision.noInputFallStartedTick = noJump.fallStartedTick;
        decision.noInputLandedTick = noJump.landedTick;
        decision.noInputLandedSafely = noJump.landedSafely;
        decision.noInputLandingSupportUID = noJump.landingSupportUID;
        decision.predictedDeathTick = noJump.deathTick;
        decision.collisionApproximation = noJump.finalState.worstApproximation;
        decision.noJumpDeathReason = noJump.deathReason;
        decision.noJumpHazardCollision = noJump.hazardCollision;
        decision.proactivePlanTriggered = std::isfinite(decision.timeToHazard)
            && decision.nearestHazardDistance >= 0.f
            && decision.timeToHazard <= static_cast<float>(decision.proactivePlanTicks);

        bool dropScenario = noJump.supportLostTick >= 0 || noJump.fallStartedTick >= 0;
        bool obviousComplex = preComplex || dropScenario;

        decision.localHazardWindowTicks = std::clamp(
            std::isfinite(decision.timeToHazard) && decision.timeToHazard >= 0.f
                ? static_cast<int>(std::ceil(decision.timeToHazard)) + 30
                : ((noJump.deathTick >= 0) ? noJump.deathTick + 30 : 30),
            0,
            std::max(0, horizonTicks - 1)
        );

        auto survivesLocalHazard = [&](CubeSimulationResult const& candidate) {
            if (!candidate.dies) {
                return true;
            }
            if (candidate.deathTick >= decision.localHazardWindowTicks) {
                return true;
            }
            if (noJump.deathTick >= 0 && candidate.deathTick >= noJump.deathTick + 20) {
                return true;
            }
            if (noJump.deathReason == "hazard" && candidate.deathReason != "hazard" && noJump.deathTick >= 0) {
                return candidate.deathTick >= noJump.deathTick + 8;
            }
            return false;
        };

        auto tapZero = simulateCubeCandidate(snapshot, nearby, config, CubeCandidate { { 0 }, false, "tap-zero" }, horizonTicks, false, &decision);
        decision.tap0DeathTick = tapZero.deathTick;
        decision.tap0DeathReason = tapZero.deathReason;
        decision.tap0HazardCollision = tapZero.hazardCollision;
        decision.tap0ApexY = tapZero.apexY;
        decision.tap0ApexTick = tapZero.apexTick;
        decision.tap0LandedTick = tapZero.landedTick;
        decision.tap0SurvivesLocalHazard = survivesLocalHazard(tapZero);
        decision.tapZeroSurvivesFullHorizon = !tapZero.dies;

        if (elapsedMs() > kPlannerHardBudgetMs) {
            decision.plannerBudgetExceeded = true;
            budgetCooldownActive = true;
            decision.expensivePlanningReason = "no-jump-simulation";
        }

        bool simpleFlatSpike = snapshot.onGround
            && !snapshot.gravFlip
            && !snapshot.onSlope
            && !dropScenario
            && decision.supportSource != SupportSource::None
            && decision.nearbyInteractives == 0
            && immediateGroundHazardCount > 0
            && noJump.deathTick >= 0
            && noJump.deathReason == "hazard";

        bool physicsFallbackCandidate = simpleFlatSpike
            && decision.nearestHazardLikelySpike
            && std::isfinite(decision.nearestHazardDistance)
            && decision.nearestHazardDistance >= 50.f
            && decision.nearestHazardDistance <= 120.f
            && tapZero.deathReason == "hazard"
            && std::abs(tapZero.deathTick - noJump.deathTick) <= 2;

        if (budgetCooldownActive || decision.plannerBudgetExceeded) {
            decision.usedFastPath = simpleFlatSpike;
            decision.usedBudgetFallback = true;
            decision.localTapZeroIsSafe = decision.tap0SurvivesLocalHazard;
            decision.localSafeTapCount = decision.localTapZeroIsSafe ? 1 : 0;
            decision.localEarliestSafeTap = decision.localTapZeroIsSafe ? 0 : -1;
            decision.localLatestSafeTap = decision.localTapZeroIsSafe ? 0 : -1;
            decision.localLatestSafeWithinSafetyThreshold = decision.localTapZeroIsSafe;
            decision.pressNowBecauseLocalSafetyThreshold = decision.localTapZeroIsSafe;
            decision.tapZeroIsSafe = decision.localTapZeroIsSafe;
            decision.safeTapCount = decision.localTapZeroIsSafe ? 1 : 0;
            decision.earliestSafeTap = decision.localTapZeroIsSafe ? 0 : -1;
            decision.latestSafeTap = decision.localTapZeroIsSafe ? 0 : -1;
            decision.latestSafeWithinSafetyThreshold = decision.localTapZeroIsSafe;
            decision.pressNowBecauseSafetyThreshold = decision.localTapZeroIsSafe;
            decision.immediatePressRequired = decision.localTapZeroIsSafe;
            decision.selectedTap = decision.localTapZeroIsSafe ? 0 : std::max(1, noJump.deathTick);
            decision.chosenDelay = decision.selectedTap;
            decision.selectedTapLabel = decision.localTapZeroIsSafe ? "fast-path-press-now" : "fast-path-wait";
            decision.selectedDecisionSource = decision.usedBudgetFallback ? "emergency" : "local-hazard-safe";
            decision.shouldPress = decision.localTapZeroIsSafe;
            if (!decision.shouldPress && physicsFallbackCandidate) {
                decision.shouldPress = true;
                decision.selectedTap = 0;
                decision.chosenDelay = 0;
                decision.selectedTapLabel = "physics-fallback-press-now";
                decision.selectedDecisionSource = "physics-fallback";
                decision.reason = "cube-flat-spike-physics-fallback";
                decision.selectedTapGrounded = true;
                return finalizeDecision("simple-flat-spike-fallback");
            }
            decision.reason = decision.shouldPress
                ? (decision.usedBudgetFallback ? "planner-budget-exceeded" : "cube-fast-flat-spike")
                : (decision.usedBudgetFallback ? "planner-budget-exceeded" : "cube-fast-flat-spike-wait");
            return finalizeDecision(decision.usedBudgetFallback ? "budget-cooldown" : "simple-flat-spike");
        }

        if (!noJump.dies && !decision.proactivePlanTriggered) {
            decision.noInputWon = true;
            decision.reason = std::isfinite(decision.nearestHazardDistance) ? "cube-hazard-wait" : "cube-clear";
            return finalizeDecision("no-input-safe");
        }

        bool noInputSafeForNow = noJump.deathTick < 0 || noJump.deathTick > kCubeDropWaitImminentThreshold;
        bool immediateHazardCollision = noJump.deathReason == "hazard" && noJump.deathTick >= 0 && noJump.deathTick <= 4;
        decision.dropGateCheckedBeforeJumpSelection = dropScenario;
        decision.dropGateComparedTapZero = dropScenario;
        decision.dropGateRejectedReason = dropScenario ? "not-safe-for-now" : "not-drop-scenario";
        decision.tapZeroBeatsNoJump = false;
        if (dropScenario) {
            bool tapZeroSurvivesFullHorizon = !tapZero.dies;
            bool tapZeroSurvivesSoonHazard = decision.tap0SurvivesLocalHazard && noJump.deathReason == "hazard";
            bool tapZeroAvoidsHazard = noJump.deathReason == "hazard" && tapZero.deathReason != "hazard";
            bool tapZeroImprovesByMargin = noJump.deathTick >= 0
                && tapZero.deathTick >= 0
                && tapZero.deathTick > noJump.deathTick + 20;
            bool immediateGroundedJumpImproves = snapshot.onGround && tapZero.firstTapGrounded
                && (tapZeroSurvivesFullHorizon || tapZeroAvoidsHazard || tapZeroImprovesByMargin || tapZeroSurvivesSoonHazard);

            decision.tapZeroBeatsNoJump = tapZeroSurvivesFullHorizon || tapZeroAvoidsHazard || tapZeroImprovesByMargin || immediateGroundedJumpImproves;

            if (tapZeroSurvivesFullHorizon) {
                decision.dropGateRejectedReason = "tap-zero-survives";
            }
            else if (tapZeroSurvivesSoonHazard) {
                decision.dropGateRejectedReason = "tap-zero-survives";
            }
            else if (tapZeroAvoidsHazard || tapZeroImprovesByMargin) {
                decision.dropGateRejectedReason = "tap-zero-beats-nojump";
            }
            else if (noJump.deathReason == "hazard" && noJump.deathTick >= 0 && noJump.deathTick <= decision.configuredSafetyTicks + 12) {
                decision.dropGateRejectedReason = "nojump-dies-hazard";
            }
            else if (immediateGroundedJumpImproves) {
                decision.dropGateRejectedReason = "immediate-jump-required";
            }
            else if (immediateHazardCollision) {
                decision.dropGateRejectedReason = "immediate-hazard";
            }
            else if (!noInputSafeForNow) {
                decision.dropGateRejectedReason = "imminent-death";
            }
            else {
                decision.dropGateAccepted = true;
                decision.dropGateRejectedReason = "accepted";
                decision.noPressGateUsed = true;
                decision.dropWaitGateUsed = true;
                decision.noPressGateReason = "drop-safe-for-now";
                decision.noInputRejectedReason = "safe-for-now-drop-replan";
                decision.immediateTapSelectionReason = "drop-safe-for-now";
                decision.selectedTap = -1;
                decision.selectedTapLabel = "safe-drop-replan";
                decision.reason = "cube-safe-drop-replan";
                return finalizeDecision("drop-scenario");
            }
        }
        decision.noPressGateReason = decision.dropGateRejectedReason;

        bool hasOrbCandidate = !nearby.orbs.empty();
        int maxCandidates = obviousComplex || hasOrbCandidate
            ? kPlannerComplexMaxCandidates
            : kPlannerNormalMaxCandidates;

        auto addDelay = [horizonTicks](std::vector<int>& delays, int tick) {
            if (tick < 0 || tick >= horizonTicks) return;
            delays.push_back(tick);
        };
        auto addWindow = [&](std::vector<int>& delays, int center, std::initializer_list<int> offsets) {
            for (int offset : offsets) {
                addDelay(delays, center + offset);
            }
        };

        std::vector<int> delaySamples;
        delaySamples.reserve(48);
        addDelay(delaySamples, 0);
        if (noJump.deathTick >= 0) {
            int triggerTick = std::max(0, noJump.deathTick - decision.configuredSafetyTicks);
            addWindow(delaySamples, triggerTick, { -2, -1, 0, 1, 2, 4 });
            addWindow(delaySamples, noJump.deathTick, { -4, -2, -1, 0 });
        }
        if (std::isfinite(decision.timeToHazard)) {
            int hazardTick = static_cast<int>(std::floor(decision.timeToHazard));
            addWindow(delaySamples, hazardTick, {
                -decision.configuredSafetyTicks,
                -std::max(1, decision.configuredSafetyTicks / 2),
                -6, -5, -4, -3, -2, -1, 0, 1
            });
            if (simpleFlatSpike) {
                for (int tick = std::max(0, hazardTick - 24); tick <= std::min(horizonTicks - 1, hazardTick + 2); ++tick) {
                    addDelay(delaySamples, tick);
                }
            }
        }
        addDelay(delaySamples, noJump.supportLostTick);
        addDelay(delaySamples, noJump.supportLostTick + 1);
        addDelay(delaySamples, noJump.supportLostTick + 2);
        addDelay(delaySamples, noJump.landedTick + 1);
        addDelay(delaySamples, noJump.landedTick + 2);
        addDelay(delaySamples, noJump.landedTick + 4);

        int orbSamples = 0;
        for (auto const* object : nearby.orbs) {
            if (!object) continue;
            int orbTick = static_cast<int>(std::lround((object->bounds.left - playerFront) / safeXSpeed));
            addWindow(delaySamples, orbTick, { -1, 0, 1 });
            if (++orbSamples >= 4) break;
        }

        if (obviousComplex) {
            for (int tick = 6; tick < std::min(horizonTicks, 48); tick += 6) {
                addDelay(delaySamples, tick);
            }
        }

        std::sort(delaySamples.begin(), delaySamples.end());
        delaySamples.erase(std::unique(delaySamples.begin(), delaySamples.end()), delaySamples.end());
        if (!delaySamples.empty()) {
            decision.maxDelayTested = delaySamples.back();
        }

        std::vector<CubeSimulationResult> candidates;
        candidates.reserve(static_cast<size_t>(maxCandidates) + 1u);
        candidates.push_back(noJump);

        auto candidateLabelForDelay = [&noJump](int delay, bool activateOrb) {
            std::string label = "tap-after-delay";

            if (noJump.supportLostTick >= 0) {
                if (delay == noJump.supportLostTick || delay == noJump.supportLostTick + 1) {
                    label = "jump-after-support-loss";
                }
                else if (delay > noJump.supportLostTick && (noJump.landedTick < 0 || delay < noJump.landedTick)) {
                    label = "drop-then-tap";
                }
            }

            if (noJump.landedTick >= 0) {
                if (delay == noJump.landedTick + 1 || delay == noJump.landedTick + 2 || delay == noJump.landedTick + 4) {
                    label = "drop-then-jump-on-landing";
                }
                else if (delay > noJump.landedTick) {
                    label = "jump-after-safe-drop";
                }
            }

            if (activateOrb) {
                label += "-orb";
            }
            return label;
        };

        auto pushCandidate = [&](CubeCandidate const& candidate) {
            if (static_cast<int>(candidates.size()) >= maxCandidates) {
                decision.plannerBudgetExceeded = true;
                decision.expensivePlanningReason = "candidate-limit";
                ++decision.earlyExitCount;
                return false;
            }

            candidates.push_back(simulateCubeCandidate(snapshot, nearby, config, candidate, horizonTicks, false, &decision));
            if (elapsedMs() > kPlannerHardBudgetMs) {
                decision.plannerBudgetExceeded = true;
                decision.expensivePlanningReason = "hard-budget";
                ++decision.earlyExitCount;
                return false;
            }
            return true;
        };

        for (int delay : delaySamples) {
            if (!pushCandidate(CubeCandidate { { delay }, false, candidateLabelForDelay(delay, false) })) {
                break;
            }
            if (hasOrbCandidate && !pushCandidate(CubeCandidate { { delay }, true, candidateLabelForDelay(delay, true) })) {
                break;
            }
        }

        if (!decision.plannerBudgetExceeded && (obviousComplex || dropScenario)) {
            auto oneTapResults = candidates;
            std::sort(oneTapResults.begin(), oneTapResults.end(), [](CubeSimulationResult const& a, CubeSimulationResult const& b) {
                if (a.dies != b.dies) return !a.dies;
                if (a.score != b.score) return a.score > b.score;
                return a.deathTick > b.deathTick;
            });

            size_t expandedCount = std::min<size_t>(dropScenario ? 6u : 4u, oneTapResults.size());
            for (size_t i = 0; i < expandedCount && !decision.plannerBudgetExceeded; ++i) {
                auto const& base = oneTapResults[i];
                if (base.candidate.tapTicks.empty()) continue;

                std::vector<int> secondTapTicks;
                addDelay(secondTapTicks, base.landedTick + 1);
                addDelay(secondTapTicks, base.landedTick + 2);
                addDelay(secondTapTicks, base.landedTick + 4);
                addDelay(secondTapTicks, base.deathTick - 1);
                std::sort(secondTapTicks.begin(), secondTapTicks.end());
                secondTapTicks.erase(std::unique(secondTapTicks.begin(), secondTapTicks.end()), secondTapTicks.end());

                for (int secondTap : secondTapTicks) {
                    if (secondTap <= base.candidate.tapTicks.front()) continue;
                    if (!pushCandidate(CubeCandidate { { base.candidate.tapTicks.front(), secondTap }, false, "tap-then-tap" })) {
                        break;
                    }
                }
            }
        }

        decision.candidateCount = static_cast<int>(candidates.size());
        decision.maxTapCountTested = 2;

        auto findSingleTapResult = [&](int tap, bool requireFullSurvival) -> CubeSimulationResult const* {
            CubeSimulationResult const* best = nullptr;
            for (auto const& candidateResult : candidates) {
                if (candidateResult.candidate.tapTicks.size() != 1) continue;
                if (candidateResult.candidate.tapTicks.front() != tap) continue;
                if (requireFullSurvival && candidateResult.dies) continue;
                if (!best
                    || candidateResult.score > best->score + 0.01f
                    || (std::abs(candidateResult.score - best->score) <= 0.01f
                        && candidateResult.deathTick > best->deathTick)) {
                    best = &candidateResult;
                }
            }
            return best;
        };

        for (auto& candidateResult : candidates) {
            candidateResult.requiresFutureTap = candidateResult.candidate.tapTicks.size() > 1;
            candidateResult.futureTapCount = candidateResult.candidate.tapTicks.size() > 1
                ? static_cast<int>(candidateResult.candidate.tapTicks.size()) - 1
                : 0;
            if (candidateResult.requiresFutureTap && !candidateResult.candidate.tapTicks.empty()) {
                if (auto const* firstTapOnly = findSingleTapResult(candidateResult.candidate.tapTicks.front(), false)) {
                    candidateResult.firstTapOnlyDies = firstTapOnly->dies;
                }
            }
        }

        std::vector<CubeSimulationResult const*> allSafeJumpCandidates;
        allSafeJumpCandidates.reserve(candidates.size());
        std::vector<CubeSimulationResult const*> safeJumpCandidates;
        safeJumpCandidates.reserve(candidates.size());
        std::vector<CubeSimulationResult const*> localSafeTapCandidates;
        localSafeTapCandidates.reserve(candidates.size());
        std::vector<CubeSimulationResult const*> decisionCandidates;
        decisionCandidates.reserve(candidates.size());

        for (auto const& candidateResult : candidates) {
            bool committedDecisionCandidate = candidateResult.candidate.tapTicks.empty()
                || !candidateResult.requiresFutureTap
                || decision.futureTapCommitEnabled;
            if (committedDecisionCandidate) {
                decisionCandidates.push_back(&candidateResult);
            }
            else if (candidateResult.candidate.tapTicks.size() > 1
                && candidateResult.candidate.tapTicks.front() == 0
                && decision.selectedRejectedReason.empty()) {
                decision.selectedRejectedReason = "rejected-uncommitted-future-tap";
            }

            if (candidateResult.candidate.tapTicks.empty()) continue;
            if (candidateResult.dies) continue;
            if (!candidateResult.firstTapGrounded && !candidateResult.candidate.activateFirstOrb) continue;
            allSafeJumpCandidates.push_back(&candidateResult);
            if (committedDecisionCandidate) {
                safeJumpCandidates.push_back(&candidateResult);
            }
        }

        for (auto const* candidateResult : decisionCandidates) {
            if (candidateResult->candidate.tapTicks.empty()) continue;
            if (!candidateResult->firstTapGrounded && !candidateResult->candidate.activateFirstOrb) continue;
            if (survivesLocalHazard(*candidateResult)) {
                localSafeTapCandidates.push_back(candidateResult);
            }
        }

        decision.safeJumpCandidatesTotal = static_cast<int>(allSafeJumpCandidates.size());

        if (!localSafeTapCandidates.empty()) {
            std::vector<int> localSafeTapTimings;
            localSafeTapTimings.reserve(localSafeTapCandidates.size());
            for (auto const* candidate : localSafeTapCandidates) {
                localSafeTapTimings.push_back(candidate->candidate.tapTicks.front());
            }
            std::sort(localSafeTapTimings.begin(), localSafeTapTimings.end());
            localSafeTapTimings.erase(std::unique(localSafeTapTimings.begin(), localSafeTapTimings.end()), localSafeTapTimings.end());
            decision.localSafeTapCount = static_cast<int>(localSafeTapTimings.size());
            decision.localEarliestSafeTap = localSafeTapTimings.front();
            decision.localLatestSafeTap = localSafeTapTimings.back();
            decision.localTapZeroIsSafe = std::binary_search(localSafeTapTimings.begin(), localSafeTapTimings.end(), 0);
            decision.localLatestSafeWithinSafetyThreshold = decision.localTapZeroIsSafe
                && decision.localLatestSafeTap <= decision.configuredSafetyTicks;
            decision.pressNowBecauseLocalSafetyThreshold = decision.localLatestSafeWithinSafetyThreshold;
        }

        std::vector<CubeSimulationResult> clearanceEvaluations;
        std::vector<CubeSimulationResult const*> evaluatedSafeCandidates;
        auto assignSelectedCandidateMetadata = [&](CubeSimulationResult const* selected) {
            if (!selected) {
                return;
            }
            decision.selectedRequiresFutureTap = selected->requiresFutureTap;
            decision.selectedFutureTapCount = selected->futureTapCount;
            decision.selectedSecondTap = selected->candidate.tapTicks.size() > 1
                ? selected->candidate.tapTicks[1]
                : -1;
            if (selected->requiresFutureTap && !decision.futureTapCommitEnabled && decision.selectedRejectedReason.empty()) {
                decision.selectedRejectedReason = "rejected-uncommitted-future-tap";
            }
        };

        if (!safeJumpCandidates.empty()) {
            auto rankedSafeCandidates = safeJumpCandidates;
            std::sort(rankedSafeCandidates.begin(), rankedSafeCandidates.end(), [](CubeSimulationResult const* a, CubeSimulationResult const* b) {
                if (a->score != b->score) return a->score > b->score;
                return a->candidate.tapTicks.front() < b->candidate.tapTicks.front();
            });

            std::unordered_set<int> shortlistedTaps;
            clearanceEvaluations.reserve(std::min<std::size_t>(kPlannerClearanceShortlist, rankedSafeCandidates.size()));
            for (auto const* candidate : rankedSafeCandidates) {
                int tap = candidate->candidate.tapTicks.front();
                if (!shortlistedTaps.insert(tap).second) continue;
                clearanceEvaluations.push_back(simulateCubeCandidate(snapshot, nearby, config, candidate->candidate, horizonTicks, true, &decision));
                if (clearanceEvaluations.size() >= kPlannerClearanceShortlist || elapsedMs() > kPlannerHardBudgetMs) {
                    break;
                }
            }

            for (auto const& candidate : clearanceEvaluations) {
                if (!candidate.dies) {
                    evaluatedSafeCandidates.push_back(&candidate);
                }
            }
        }

        if (!safeJumpCandidates.empty()) {
            std::vector<int> safeTapTimings;
            safeTapTimings.reserve(safeJumpCandidates.size());
            for (auto const* candidate : safeJumpCandidates) {
                safeTapTimings.push_back(candidate->candidate.tapTicks.front());
            }
            std::sort(safeTapTimings.begin(), safeTapTimings.end());
            safeTapTimings.erase(std::unique(safeTapTimings.begin(), safeTapTimings.end()), safeTapTimings.end());

            decision.safeTapTimingsBeforeFiltering = static_cast<int>(safeTapTimings.size());
            decision.safeTapTimingsAfterFiltering = static_cast<int>(evaluatedSafeCandidates.empty() ? safeTapTimings.size() : evaluatedSafeCandidates.size());

            decision.safeTapCount = static_cast<int>(safeTapTimings.size());
            decision.earliestSafeTap = safeTapTimings.front();
            decision.latestSafeTap = safeTapTimings.back();
            decision.safeWindowWidth = decision.latestSafeTap - decision.earliestSafeTap;
            decision.tapZeroIsSafe = std::binary_search(safeTapTimings.begin(), safeTapTimings.end(), 0);
            decision.latestSafeWithinSafetyThreshold = decision.tapZeroIsSafe
                && decision.latestSafeTap <= decision.configuredSafetyTicks;
            decision.pressNowBecauseSafetyThreshold = decision.latestSafeWithinSafetyThreshold;

            for (size_t i = 1; i < safeTapTimings.size(); ++i) {
                if (safeTapTimings[i] != safeTapTimings[i - 1] + 1) {
                    decision.safeTapSparse = true;
                    break;
                }
            }

            decision.framePerfectFallbackUsed = decision.safeTapCount == 1;
            decision.effectiveSafetyTicks = decision.configuredSafetyTicks;

            auto bestCandidateForTap = [&](int tap) -> CubeSimulationResult const* {
                if (auto const* singleTap = findSingleTapResult(tap, true)) {
                    return singleTap;
                }
                CubeSimulationResult const* best = nullptr;
                auto const& preferredPool = evaluatedSafeCandidates.empty() ? safeJumpCandidates : evaluatedSafeCandidates;
                for (auto const* candidate : preferredPool) {
                    if (candidate->candidate.tapTicks.empty() || candidate->candidate.tapTicks.front() != tap) continue;
                    if (!best
                        || candidate->minHazardClearance > best->minHazardClearance + 0.01f
                        || (std::abs(candidate->minHazardClearance - best->minHazardClearance) <= 0.01f && candidate->score > best->score + 0.01f)
                        || (std::abs(candidate->minHazardClearance - best->minHazardClearance) <= 0.01f
                            && std::abs(candidate->score - best->score) <= 0.01f
                            && !candidate->candidate.activateFirstOrb
                            && best->candidate.activateFirstOrb)) {
                        best = candidate;
                    }
                }
                if (best) return best;

                for (auto const* candidate : safeJumpCandidates) {
                    if (!candidate->candidate.tapTicks.empty() && candidate->candidate.tapTicks.front() == tap) {
                        return candidate;
                    }
                }
                return nullptr;
            };

            CubeSimulationResult const* selected = nullptr;
            CubeSimulationResult const* tapZeroCandidate = decision.tapZeroIsSafe ? bestCandidateForTap(0) : nullptr;
            CubeSimulationResult const* latestCandidate = decision.tapZeroIsSafe ? bestCandidateForTap(decision.latestSafeTap) : nullptr;
            bool pressNowBecauseZeroPreferred = false;
            if (tapZeroCandidate && latestCandidate && latestCandidate != tapZeroCandidate) {
                float clearanceGain = tapZeroCandidate->minHazardClearance - latestCandidate->minHazardClearance;
                float scoreGain = tapZeroCandidate->score - latestCandidate->score;
                pressNowBecauseZeroPreferred = clearanceGain > 2.0f || scoreGain > 60.f;
            }

            if (decision.tapZeroIsSafe && (decision.pressNowBecauseSafetyThreshold || pressNowBecauseZeroPreferred)) {
                selected = tapZeroCandidate ? tapZeroCandidate : bestCandidateForTap(0);
                decision.immediatePressRequired = true;
                decision.immediateTapSelectionReason = decision.pressNowBecauseSafetyThreshold
                    ? "safety-threshold-press-now"
                    : "tap-zero-better-future-press-now";
            }
            else if (decision.tapZeroIsSafe) {
                selected = latestCandidate ? latestCandidate : bestCandidateForTap(decision.latestSafeTap);
                decision.immediateTapSelectionReason = "safe-window-open-wait";
            }
            else {
                selected = bestCandidateForTap(decision.earliestSafeTap);
                decision.immediateTapSelectionReason = "tap-zero-not-safe-wait";
            }

            if (!selected) {
                selected = safeJumpCandidates.front();
            }

            assignSelectedCandidateMetadata(selected);
            decision.targetSafeTap = selected->candidate.tapTicks.empty() ? -1 : selected->candidate.tapTicks.front();
            decision.selectedTapFromFullSafeSet = decision.targetSafeTap;
            decision.selectedTapAfterBuffer = decision.targetSafeTap;
            decision.selectedTap = decision.targetSafeTap;
            decision.chosenDelay = decision.selectedTap;
            decision.selectedTapScore = selected->score;
            decision.selectedTapClearance = selected->minHazardClearance;
            decision.selectedTapClearanceComputed = selected->hazardClearanceComputed;
            decision.selectedTapClearanceApproximate = selected->hazardClearanceApproximate;
            decision.selectedTapLabel = selected->candidate.label;
            decision.predictedDeathTick = selected->deathTick;
            decision.activatedOrbUID = selected->activatedOrbUID;
            decision.collisionApproximation = selected->finalState.worstApproximation;
            decision.bestDeathReason = selected->deathReason;
            decision.selectedHazardCollision = selected->hazardCollision;
            decision.selectedTapGrounded = selected->firstTapGrounded;
            decision.selectedApexY = selected->apexY;
            decision.selectedApexTick = selected->apexTick;
            decision.selectedLandedTick = selected->landedTick;
            decision.noInputRejectedReason = noJump.dies
                ? (noJump.deathReason.empty() ? "eventual-death" : noJump.deathReason)
                : "proactive-plan";
            decision.shouldPress = decision.pressNowBecauseSafetyThreshold
                && decision.selectedTap == 0
                && (!decision.selectedRequiresFutureTap || decision.futureTapCommitEnabled);
            decision.reason = decision.plannerBudgetExceeded
                ? "planner-budget-exceeded"
                : (decision.shouldPress ? "cube-hazard" : "cube-hazard-wait");
            decision.selectedDecisionSource = "full-horizon-safe";
            return finalizeDecision("safe-candidate-selection");
        }

        if (!localSafeTapCandidates.empty()) {
            auto bestLocalCandidateForTap = [&](int tap) -> CubeSimulationResult const* {
                if (auto const* singleTap = findSingleTapResult(tap, false)) {
                    if (survivesLocalHazard(*singleTap)) {
                        return singleTap;
                    }
                }

                CubeSimulationResult const* best = nullptr;
                for (auto const* candidate : localSafeTapCandidates) {
                    if (candidate->candidate.tapTicks.empty() || candidate->candidate.tapTicks.front() != tap) continue;
                    if (!best
                        || candidate->score > best->score + 0.01f
                        || (std::abs(candidate->score - best->score) <= 0.01f
                            && candidate->deathTick > best->deathTick)) {
                        best = candidate;
                    }
                }
                return best;
            };

            CubeSimulationResult const* selectedLocal = nullptr;
            CubeSimulationResult const* localTapZeroCandidate = decision.localTapZeroIsSafe ? bestLocalCandidateForTap(0) : nullptr;
            CubeSimulationResult const* localLatestCandidate = decision.localTapZeroIsSafe ? bestLocalCandidateForTap(decision.localLatestSafeTap) : nullptr;
            bool pressLocalNowBecauseZeroPreferred = false;
            if (localTapZeroCandidate && localLatestCandidate && localLatestCandidate != localTapZeroCandidate) {
                pressLocalNowBecauseZeroPreferred = localTapZeroCandidate->score > localLatestCandidate->score + 60.f;
            }

            if (decision.localTapZeroIsSafe && (decision.pressNowBecauseLocalSafetyThreshold || pressLocalNowBecauseZeroPreferred)) {
                selectedLocal = localTapZeroCandidate ? localTapZeroCandidate : bestLocalCandidateForTap(0);
            }
            else if (decision.localTapZeroIsSafe) {
                selectedLocal = localLatestCandidate ? localLatestCandidate : bestLocalCandidateForTap(decision.localLatestSafeTap);
            }
            else {
                selectedLocal = bestLocalCandidateForTap(decision.localEarliestSafeTap);
            }

            if (!selectedLocal) {
                selectedLocal = localSafeTapCandidates.front();
            }

            assignSelectedCandidateMetadata(selectedLocal);
            decision.selectedTap = selectedLocal->candidate.tapTicks.front();
            decision.chosenDelay = decision.selectedTap;
            decision.targetSafeTap = decision.selectedTap;
            decision.selectedTapFromFullSafeSet = decision.selectedTap;
            decision.selectedTapAfterBuffer = decision.selectedTap;
            decision.selectedTapScore = selectedLocal->score;
            decision.selectedTapClearance = selectedLocal->minHazardClearance;
            decision.selectedTapClearanceComputed = selectedLocal->hazardClearanceComputed;
            decision.selectedTapClearanceApproximate = selectedLocal->hazardClearanceApproximate;
            decision.selectedTapLabel = selectedLocal->candidate.label;
            decision.predictedDeathTick = selectedLocal->deathTick;
            decision.activatedOrbUID = selectedLocal->activatedOrbUID;
            decision.collisionApproximation = selectedLocal->finalState.worstApproximation;
            decision.bestDeathReason = selectedLocal->deathReason;
            decision.selectedHazardCollision = selectedLocal->hazardCollision;
            decision.selectedTapGrounded = selectedLocal->firstTapGrounded;
            decision.selectedApexY = selectedLocal->apexY;
            decision.selectedApexTick = selectedLocal->apexTick;
            decision.selectedLandedTick = selectedLocal->landedTick;
            decision.immediatePressRequired = decision.localTapZeroIsSafe && decision.pressNowBecauseLocalSafetyThreshold;
            decision.shouldPress = decision.immediatePressRequired
                && decision.selectedTap == 0
                && (!decision.selectedRequiresFutureTap || decision.futureTapCommitEnabled);
            decision.reason = decision.shouldPress ? "cube-hazard" : "cube-hazard-wait";
            decision.selectedDecisionSource = "local-hazard-safe";
            decision.noInputRejectedReason = noJump.deathReason.empty() ? "eventual-death" : noJump.deathReason;
            return finalizeDecision("local-hazard-safe");
        }

        if (physicsFallbackCandidate) {
            decision.shouldPress = true;
            decision.selectedTap = 0;
            decision.chosenDelay = 0;
            decision.selectedTapLabel = "physics-fallback-press-now";
            decision.selectedDecisionSource = "physics-fallback";
            decision.selectedTapGrounded = true;
            decision.reason = "cube-flat-spike-physics-fallback";
            return finalizeDecision("post-search-physics-fallback");
        }

        auto isBetter = [](CubeSimulationResult const& lhs, CubeSimulationResult const& rhs) {
            if (lhs.dies != rhs.dies) return !lhs.dies;
            if (!lhs.dies && !rhs.dies) {
                if (lhs.score != rhs.score) return lhs.score > rhs.score;
                int lhsFirstTap = lhs.candidate.tapTicks.empty() ? std::numeric_limits<int>::max() : lhs.candidate.tapTicks.front();
                int rhsFirstTap = rhs.candidate.tapTicks.empty() ? std::numeric_limits<int>::max() : rhs.candidate.tapTicks.front();
                return lhsFirstTap > rhsFirstTap;
            }
            if (lhs.deathTick != rhs.deathTick) {
                return lhs.deathTick > rhs.deathTick;
            }
            return lhs.score > rhs.score;
        };

        CubeSimulationResult best = noJump;
        for (auto const* candidate : decisionCandidates) {
            if (candidate && isBetter(*candidate, best)) {
                best = *candidate;
            }
        }

        assignSelectedCandidateMetadata(&best);
        decision.chosenDelay = best.candidate.tapTicks.empty() ? -1 : best.candidate.tapTicks.front();
        decision.selectedTap = decision.chosenDelay;
        decision.predictedDeathTick = best.deathTick;
        decision.activatedOrbUID = best.activatedOrbUID;
        decision.collisionApproximation = best.finalState.worstApproximation;
        decision.selectedTapScore = best.score;
        decision.selectedTapClearance = best.minHazardClearance;
        decision.selectedTapClearanceComputed = best.hazardClearanceComputed;
        decision.selectedTapClearanceApproximate = best.hazardClearanceApproximate;
        decision.selectedTapLabel = best.candidate.label;
        decision.noJumpDeathReason = noJump.deathReason;
        decision.bestDeathReason = best.deathReason;
        decision.noInputRejectedReason = noJump.deathReason.empty() ? "eventual-death" : noJump.deathReason;
        decision.targetSafeTap = decision.selectedTap;
        decision.selectedTapFromFullSafeSet = decision.selectedTap;
        decision.selectedTapAfterBuffer = decision.selectedTap;
        decision.selectedHazardCollision = best.hazardCollision;
        decision.selectedTapGrounded = best.firstTapGrounded;
        decision.selectedApexY = best.apexY;
        decision.selectedApexTick = best.apexTick;
        decision.selectedLandedTick = best.landedTick;

        if (!best.dies) {
            decision.shouldPress = !best.candidate.tapTicks.empty()
                && best.candidate.tapTicks.front() == 0
                && (!best.requiresFutureTap || decision.futureTapCommitEnabled);
            decision.noInputWon = best.candidate.tapTicks.empty();
            decision.immediateTapSelectionReason = decision.shouldPress
                ? "best-safe-or-longest-survivor-immediate"
                : "best-safe-or-longest-survivor-delayed";
            decision.reason = decision.plannerBudgetExceeded
                ? "planner-budget-exceeded"
                : (decision.shouldPress ? "cube-hazard" : "cube-hazard-wait");
            decision.selectedDecisionSource = "full-horizon-safe";
            return finalizeDecision("best-safe-survivor");
        }

        if (noJump.deathTick >= 0 && noJump.deathTick <= 4) {
            decision.shouldPress = !best.candidate.tapTicks.empty()
                && best.candidate.tapTicks.front() == 0
                && (!best.requiresFutureTap || decision.futureTapCommitEnabled);
            decision.emergency = true;
            decision.immediatePressRequired = decision.shouldPress;
            decision.immediateTapSelectionReason = decision.shouldPress
                ? "emergency-best-effort-immediate"
                : "emergency-no-immediate-safe-tap";
            decision.reason = decision.plannerBudgetExceeded
                ? "planner-budget-exceeded"
                : "cube-hazard-emergency";
            decision.selectedDecisionSource = "emergency";
            return finalizeDecision("emergency-path");
        }

        decision.reason = decision.plannerBudgetExceeded ? "planner-budget-exceeded" : "cube-hazard-failing-all";
        decision.shouldPress = !best.candidate.tapTicks.empty()
            && best.candidate.tapTicks.front() == 0
            && (!best.requiresFutureTap || decision.futureTapCommitEnabled);
        decision.immediateTapSelectionReason = decision.shouldPress
            ? "failing-all-immediate-best-effort"
            : "failing-all-delayed-best-effort";
        decision.noInputWon = best.candidate.tapTicks.empty();
        decision.selectedDecisionSource = decision.noInputWon ? "no-input" : "emergency";
        return finalizeDecision("failing-all");
    }

    PlannerDecision planUnsupported(PlayerSnapshot const& snapshot, PlannerConfig const& config) {
        PlannerDecision decision;
        decision.valid = true;
        decision.horizonSeconds = config.horizonSeconds;
        decision.reason = config.experimentalMultiMode
            ? "experimental-mode-not-implemented"
            : "mode-unsupported";
        decision.collisionApproximation = CollisionApproximation::Unknown;
        if (snapshot.inputHeld) {
            decision.shouldPress = false;
        }
        m_lastDecision = decision;
        return decision;
    }

private:
    PlannerDecision m_lastDecision;
    int m_budgetCooldownTicks = 0;

    static SimState makeSimState(PlayerSnapshot const& snapshot, std::vector<CachedLevelObject const*> const& solids) {
        SimState state;
        state.x = snapshot.x;
        state.y = snapshot.y;
        state.xSpeed = snapshot.observedXSpeed;
        state.baseXSpeed = snapshot.baseXSpeed;
        state.currentXVelocity = snapshot.currentXVelocity;
        state.speedRatio = snapshot.speedRatio;
        state.deltaTime = snapshot.deltaTime;
        state.jumpUpdateDt = snapshot.jumpUpdateDt;
        state.yVelocity = snapshot.yVelocity;
        state.gravityPerTick = snapshot.gravityPerTick;
        state.gravityMagnitude = snapshot.gravityMagnitude;
        state.onGround = snapshot.onGround;
        state.gravFlip = snapshot.gravFlip;
        state.isMini = snapshot.isMini;
        state.isDead = snapshot.isDead;
        state.inputHeld = snapshot.inputHeld;
        state.touchedRing = snapshot.touchedRing;
        state.touchedPad = snapshot.touchedPad;
        state.jumpBuffered = snapshot.jumpBuffered;
        state.mode = snapshot.mode;
        state.worstApproximation = snapshot.gravityApproximate || snapshot.speedApproximate
            ? CollisionApproximation::FallbackAABB
            : CollisionApproximation::GameRect;

        int supportingSolidUID = CollisionProvider::findSupportingSolidUID(state, solids);
        if (supportingSolidUID >= 0) {
            state.currentSupportUID = supportingSolidUID;
            state.supportSource = SupportSource::Solid;
        }
        else if (snapshot.onGround && !snapshot.gravFlip) {
            float footY = snapshot.y - state.halfHeight();
            if (CollisionProvider::isNearLikelyDefaultGroundBand(footY)) {
                state.hasDefaultGroundSupport = true;
                state.defaultGroundY = footY;
                state.currentSupportUID = kSyntheticDefaultGroundSupportUID;
                state.supportSource = SupportSource::DefaultGround;
            }
        }

        return state;
    }

    static float jumpVelocity(SimState const& state) {
        return state.isMini ? kDefaultMiniJumpVelocity : kDefaultJumpVelocity;
    }

    static float gravityFactorForMode(GameMode mode) {
        switch (mode) {
            case GameMode::Robot:
                return 0.9f;
            case GameMode::Cube:
            default:
                return 0.6f;
        }
    }

    static float quantizeVelocity(float value) {
        return std::round(value * 1000.f) / 1000.f;
    }

    static void stepCubePhysics(SimState& state, bool hold) {
        bool jumpedThisTick = false;
        if (state.onGround && hold) {
            state.yVelocity = quantizeVelocity(jumpVelocity(state) * (state.gravFlip ? -1.f : 1.f));
            state.onGround = false;
            state.currentSupportUID = -1;
            state.supportSource = SupportSource::None;
            jumpedThisTick = true;
        }

        if (!jumpedThisTick) {
            float flipMod = state.gravFlip ? -1.f : 1.f;
            float gravityDelta = -(gravityFactorForMode(state.mode) * state.gravityMagnitude * std::max(state.jumpUpdateDt, 0.f) * flipMod);
            state.yVelocity = quantizeVelocity(state.yVelocity + gravityDelta);
            state.yVelocity = std::clamp(state.yVelocity, -kCubeTerminalVelocity, kCubeTerminalVelocity);
        }

        state.y += state.yVelocity * std::max(state.jumpUpdateDt, 0.f);
        state.x += state.xSpeed;
    }

    static void applyPortalEffect(SimState& state, CachedLevelObject const& object) {
        switch (object.portalKind) {
            case PortalKind::GravityFlip:
                state.gravFlip = true;
                state.hasDefaultGroundSupport = false;
                state.currentSupportUID = -1;
                state.supportSource = SupportSource::None;
                break;
            case PortalKind::GravityNormal:
                state.gravFlip = false;
                state.currentSupportUID = -1;
                state.supportSource = SupportSource::None;
                break;
            case PortalKind::SpeedHalf:
            case PortalKind::SpeedNormal:
            case PortalKind::SpeedDouble:
            case PortalKind::SpeedTriple:
            case PortalKind::SpeedQuad:
                state.speedRatio = object.speedValue;
                if (state.baseXSpeed > 0.f) {
                    state.xSpeed = state.baseXSpeed * state.speedRatio;
                }
                break;
            case PortalKind::ModeCube:
            case PortalKind::ModeShip:
            case PortalKind::ModeBall:
            case PortalKind::ModeUFO:
            case PortalKind::ModeWave:
            case PortalKind::ModeRobot:
            case PortalKind::ModeSpider:
            case PortalKind::ModeSwing:
                state.mode = object.targetMode;
                break;
            case PortalKind::SizeMini:
                state.isMini = true;
                break;
            case PortalKind::SizeNormal:
                state.isMini = false;
                break;
            default:
                break;
        }
    }

    static void applyPadEffect(SimState& state, CachedLevelObject const& object) {
        switch (object.padKind) {
            case PadKind::BlueGravity:
                state.gravFlip = !state.gravFlip;
                state.hasDefaultGroundSupport = false;
                break;
            default:
                break;
        }

        state.yVelocity = quantizeVelocity(jumpVelocity(state) * (state.gravFlip ? -1.f : 1.f));
        state.onGround = false;
        state.currentSupportUID = -1;
        state.supportSource = SupportSource::None;
        state.touchedPad = true;
    }

    static void applyOrbEffect(SimState& state, CachedLevelObject const& object) {
        switch (object.orbKind) {
            case OrbKind::Black:
                state.yVelocity = quantizeVelocity(-jumpVelocity(state) * (state.gravFlip ? -1.f : 1.f));
                break;

            case OrbKind::BlueGravity:
            case OrbKind::GreenGravity:
                state.gravFlip = !state.gravFlip;
                state.hasDefaultGroundSupport = false;
                state.yVelocity = quantizeVelocity(jumpVelocity(state) * (state.gravFlip ? -1.f : 1.f));
                break;

            case OrbKind::DashGreen:
            case OrbKind::DashMagenta:
                state.yVelocity = quantizeVelocity(jumpVelocity(state) * 1.2f * (state.gravFlip ? -1.f : 1.f));
                state.x += state.xSpeed * 1.5f;
                break;

            default:
                state.yVelocity = quantizeVelocity(jumpVelocity(state) * (state.gravFlip ? -1.f : 1.f));
                break;
        }

        state.onGround = false;
        state.currentSupportUID = -1;
        state.supportSource = SupportSource::None;
        state.touchedRing = true;
    }

    static bool overlaps(RectF const& playerBounds, CachedLevelObject const& object) {
        return playerBounds.overlaps(object.bounds);
    }

    static CubeSimulationResult simulateCubeCandidate(
        PlayerSnapshot const& snapshot,
        NearbyObjectSet const& nearby,
        PlannerConfig const& config,
        CubeCandidate const& candidate,
        int horizonTicks,
        bool computeClearance,
        PlannerDecision* metrics = nullptr,
        std::vector<CubeTraceSample>* traceSamples = nullptr
    ) {
        CubeSimulationResult result;
        result.candidate = candidate;

        if (metrics) {
            ++metrics->simulatedCandidateCount;
        }

        SimState state = makeSimState(snapshot, nearby.solids);
        result.hasDefaultGroundSupport = state.hasDefaultGroundSupport;
        result.defaultGroundY = state.defaultGroundY;
        result.initialSupportSource = state.supportSource;
        result.initialSupportUID = state.currentSupportUID;
        result.apexY = state.y;
        result.apexTick = 0;
        bool firstOrbConsumed = false;
        float broadLowerBound = (state.hasDefaultGroundSupport && !state.gravFlip)
            ? state.defaultGroundY - 240.f
            : snapshot.y - 720.f;
        float broadUpperBound = snapshot.y + 720.f;

        auto recordTrace = [&](int tickOffset) {
            if (!traceSamples) return;
            traceSamples->push_back(CubeTraceSample {
                tickOffset,
                state.x,
                state.y,
                state.yVelocity,
                state.onGround,
            });
        };

        recordTrace(0);

        for (int step = 0; step < horizonTicks; ++step) {
            if (metrics) {
                ++metrics->simulatedStepCount;
            }
            RectF previousBounds = CollisionProvider::playerBounds(state);
            bool hold = false;
            for (int tapTick : candidate.tapTicks) {
                if (step == tapTick) {
                    if (!candidate.tapTicks.empty() && tapTick == candidate.tapTicks.front()) {
                        result.firstTapGrounded = state.onGround;
                        if (!result.firstTapGrounded && !candidate.activateFirstOrb) {
                            result.candidate.label = "airborne-noop";
                        }
                    }
                    hold = true;
                    break;
                }
            }

            bool wasOnGround = state.onGround;
            int previousSupportUID = state.currentSupportUID;

            stepCubePhysics(state, hold);
            state.onGround = false;
            state.currentSupportUID = -1;
            state.supportSource = SupportSource::None;

            std::string deathReason;
            if (!CollisionProvider::resolveCubeSolids(state, previousBounds, nearby.solids, deathReason, metrics)) {
                result.dies = true;
                result.deathTick = step;
                result.deathReason = deathReason;
                result.finalSupportSource = state.supportSource;
                result.finalState = state;
                result.score = -500000.f + static_cast<float>(step) * 1000.f;
                recordTrace(step + 1);
                return result;
            }

            CollisionProvider::resolveDefaultGroundSupport(state, previousBounds);

            RectF currentBounds = CollisionProvider::playerBounds(state);
            auto interactiveBegin = CollisionProvider::rangeBegin(nearby.interactives, currentBounds.left - 24.f);

            for (auto it = interactiveBegin; it != nearby.interactives.end() && (*it)->bounds.left <= currentBounds.right + 24.f; ++it) {
                auto const* object = *it;
                if (!object || !overlaps(currentBounds, *object)) continue;

                state.worstApproximation = mergeApproximation(state.worstApproximation, object->collisionApproximation);

                if (object->isPad() && state.lastPadUID != object->uniqueID) {
                    state.lastPadUID = object->uniqueID;
                    applyPadEffect(state, *object);
                    currentBounds = CollisionProvider::playerBounds(state);
                    continue;
                }

                if (object->isPortal() && state.lastPortalUID != object->uniqueID) {
                    state.lastPortalUID = object->uniqueID;
                    applyPortalEffect(state, *object);
                    currentBounds = CollisionProvider::playerBounds(state);
                    if (state.mode != GameMode::Cube) {
                        result.dies = true;
                        result.deathTick = step;
                        result.deathReason = "unsupported-mode-transition";
                        result.finalSupportSource = state.supportSource;
                        result.finalState = state;
                        result.score = -250000.f + static_cast<float>(step) * 1000.f;
                        recordTrace(step + 1);
                        return result;
                    }
                    continue;
                }

                if (candidate.activateFirstOrb && object->isOrb() && !firstOrbConsumed && state.lastOrbUID != object->uniqueID) {
                    state.lastOrbUID = object->uniqueID;
                    state.activatedOrbUID = object->uniqueID;
                    firstOrbConsumed = true;
                    applyOrbEffect(state, *object);
                    currentBounds = CollisionProvider::playerBounds(state);
                }
            }

            if (wasOnGround && !state.onGround && result.supportLostTick < 0) {
                result.supportLostTick = step;
                result.fallStartedTick = step;
            }

            if (!wasOnGround && state.onGround && result.landedTick < 0) {
                result.landedTick = step;
                result.landingSupportUID = state.currentSupportUID;
                result.landedSafely = true;
            }

            if (wasOnGround && state.onGround && previousSupportUID != state.currentSupportUID && state.currentSupportUID >= 0) {
                result.landingSupportUID = state.currentSupportUID;
            }

            if ((!state.gravFlip && state.y > result.apexY) || (state.gravFlip && state.y < result.apexY)) {
                result.apexY = state.y;
                result.apexTick = step + 1;
            }

            if (CollisionProvider::touchesHazard(state, nearby.hazards, deathReason, &result.hazardCollision, metrics)) {
                if (computeClearance) {
                    CollisionProvider::updateMinHazardClearance(currentBounds, nearby.hazards, result, metrics);
                }
                result.dies = true;
                result.deathTick = step;
                result.deathReason = deathReason;
                result.finalSupportSource = state.supportSource;
                result.finalState = state;
                result.activatedOrbUID = state.activatedOrbUID;
                result.score = -400000.f + static_cast<float>(step) * 1000.f;
                recordTrace(step + 1);
                return result;
            }

            if (state.y < broadLowerBound || state.y > broadUpperBound) {
                result.dies = true;
                result.deathTick = step;
                result.deathReason = "world-bounds";
                result.finalSupportSource = state.supportSource;
                result.finalState = state;
                result.activatedOrbUID = state.activatedOrbUID;
                result.score = -350000.f + static_cast<float>(step) * 1000.f;
                recordTrace(step + 1);
                return result;
            }

            if (computeClearance) {
                CollisionProvider::updateMinHazardClearance(currentBounds, nearby.hazards, result, metrics);
            }

            recordTrace(step + 1);
        }

        if (!result.candidate.tapTicks.empty() && !result.firstTapGrounded && !result.candidate.activateFirstOrb) {
            result.candidate.label = "airborne-noop";
        }

        result.finalState = state;
        result.activatedOrbUID = state.activatedOrbUID;
        result.finalSupportSource = state.supportSource;
        result.score = state.x;
        if (result.landedSafely) {
            result.score += 120.f;
        }
        result.score -= static_cast<float>(candidate.tapTicks.size()) * 6.f;
        if (candidate.activateFirstOrb) {
            result.score -= 2.f;
        }
        if (state.worstApproximation == CollisionApproximation::FallbackAABB && !config.useApproximateCollisionFallback) {
            result.dies = true;
            result.deathReason = "fallback-collision-disabled";
            result.score = -300000.f;
        }
        return result;
    }
};

} // namespace autobot

#endif
