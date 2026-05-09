#ifndef _autobot_hpp
#define _autobot_hpp

#include <Geode/Geode.hpp>

#include "AutoBotDebug.hpp"
#include "AutoBotModel.hpp"
#include "AutoBotPlanner.hpp"
#include "../zBot.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace autobot {

class AutoBot {
public:
    static AutoBot* get() {
        static AutoBot* instance = new AutoBot();
        return instance;
    }

    void tick(GJBaseGameLayer* gl, float dt) {
        using Clock = std::chrono::steady_clock;
        auto tickStart = Clock::now();
        m_tickCounter++;

        if (!gl) return;

        auto* play = PlayLayer::get();
        if (!play || static_cast<GJBaseGameLayer*>(play) != gl) return;
        if (geode::DestructorLock::isLocked(static_cast<cocos2d::CCNode*>(play))
         || geode::DestructorLock::isLocked(static_cast<cocos2d::CCNode*>(gl))) {
            return;
        }

        auto* player = gl->m_player1;
        if (!player || geode::DestructorLock::isLocked(static_cast<cocos2d::CCNode*>(player))) {
            return;
        }

        if (player->m_isDead) {
            if (zBot::get()->autoBotFileLogging) {
                AutoBotDebug::get()->flushBufferedTickEvents("player-death");
            }
            resetState();
            return;
        }

        ensureLevelCache(gl);
        if (!m_levelCache.isBuilt()) {
            m_lastDecisionReason = "level-cache-unavailable";
            return;
        }

        PlayerSnapshot snapshot = m_stateReader.capture(gl, player, m_holding, dt);
        auto config = currentPlannerConfig();
        collectNearbyObjects(snapshot, config);
        updateValidation(snapshot);

        PlannerDecision decision = decide(snapshot, config);
        if (zBot::get()->autoBotValidateCubePhysics
            && m_validationRunsStarted < 4
            && decision.shouldPress
            && !m_holding
            && snapshot.mode == GameMode::Cube) {
            beginValidation(snapshot, config, decision);
        }
        applyDecision(gl, snapshot, decision.shouldPress);
        decision.tickTimeMs = std::chrono::duration<double, std::milli>(Clock::now() - tickStart).count();
        m_lastDecision = decision;
        m_lastDecisionReason = decision.reason;

        if (zBot::get()->autoBotFileLogging) {
            std::ostringstream line;
            line << "frame=" << snapshot.frame
                 << " mode=" << gameModeName(snapshot.mode)
                 << " x=" << snapshot.x
                 << " y=" << snapshot.y
                 << " dt=" << snapshot.deltaTime
                 << " jumpUpdateDt=" << snapshot.jumpUpdateDt
                 << " progressFrameDelta=" << snapshot.progressFrameDelta
                 << " captureTickDelta=" << snapshot.captureTickDelta
                 << " rawXDelta=" << snapshot.rawXDelta
                 << " xSpeed=" << snapshot.observedXSpeed
                 << " currentXVelocity=" << snapshot.currentXVelocity
                 << " baseXSpeed=" << snapshot.baseXSpeed
                 << " speedRatio=" << snapshot.speedRatio
                 << " observedXSpeedPerProgressFrame=" << snapshot.observedXSpeedPerProgressFrame
                 << " observedXSpeedPerSimTick=" << snapshot.observedXSpeedPerSimTick
                 << " xSpeedUsedBySimulator=" << snapshot.xSpeedUsedBySimulator
                 << " xSpeedUnitMode=" << xSpeedUnitModeName(snapshot.xSpeedUnitMode)
                 << " yVel=" << snapshot.yVelocity
                 << " gravityMagnitude=" << snapshot.gravityMagnitude
                 << " onGround=" << snapshot.onGround
                 << " gravFlip=" << snapshot.gravFlip
                 << " inputHeld=" << snapshot.inputHeld
                 << " rollingPlanner=1"
                 << " configHorizonSeconds=" << config.horizonSeconds
                 << " decisionHorizonSeconds=" << decision.horizonSeconds
                 << " cubeTimingSafetyTicks=" << config.cubeTimingSafetyTicks
                 << " hasDefaultGroundSupport=" << decision.hasDefaultGroundSupport
                 << " defaultGroundY=" << decision.defaultGroundY
                 << " supportSource=" << supportSourceName(decision.supportSource)
                 << " currentSupportUID=" << decision.currentSupportUID
                 << " supportLostTick=" << decision.noInputSupportLostTick
                 << " fallStartedTick=" << decision.noInputFallStartedTick
                 << " landedTick=" << decision.noInputLandedTick
                 << " landingSupportUID=" << decision.noInputLandingSupportUID
                 << " landedSafely=" << decision.noInputLandedSafely
                 << " proactivePlanTriggered=" << decision.proactivePlanTriggered
                 << " proactivePlanTicks=" << decision.proactivePlanTicks
                 << " nearestHazardDistance=" << decision.nearestHazardDistance
                 << " nearestHazardObjectID=" << decision.nearestHazardObjectID
                 << " nearestHazardObjectUID=" << decision.nearestHazardObjectUID
                 << " nearestHazardLikelySpike=" << decision.nearestHazardLikelySpike
                 << " nearestHazardBounds=" << decision.nearestHazardLeft << "," << decision.nearestHazardRight << "," << decision.nearestHazardBottom << "," << decision.nearestHazardTop
                 << " nearestHazardApproximation=" << collisionApproximationName(decision.nearestHazardApproximation)
                 << " timeToHazard=" << decision.timeToHazard
                 << " tapZeroIsSafe=" << decision.tapZeroIsSafe
                 << " localTapZeroIsSafe=" << decision.localTapZeroIsSafe
                 << " safeTapCount=" << decision.safeTapCount
                 << " localSafeTapCount=" << decision.localSafeTapCount
                 << " latestSafeTap=" << decision.latestSafeTap
                 << " earliestSafeTap=" << decision.earliestSafeTap
                 << " localLatestSafeTap=" << decision.localLatestSafeTap
                 << " localEarliestSafeTap=" << decision.localEarliestSafeTap
                 << " latestSafeWithinSafetyThreshold=" << decision.latestSafeWithinSafetyThreshold
                 << " pressNowBecauseSafetyThreshold=" << decision.pressNowBecauseSafetyThreshold
                 << " localLatestSafeWithinSafetyThreshold=" << decision.localLatestSafeWithinSafetyThreshold
                 << " pressNowBecauseLocalSafetyThreshold=" << decision.pressNowBecauseLocalSafetyThreshold
                 << " dropWaitGateUsed=" << decision.dropWaitGateUsed
                 << " dropGateCheckedBeforeJumpSelection=" << decision.dropGateCheckedBeforeJumpSelection
                 << " dropGateComparedTapZero=" << decision.dropGateComparedTapZero
                 << " tapZeroBeatsNoJump=" << decision.tapZeroBeatsNoJump
                 << " dropGateAccepted=" << decision.dropGateAccepted
                 << " dropGateRejectedReason=" << decision.dropGateRejectedReason
                 << " noPressGateReason=" << decision.noPressGateReason
                 << " noInputRejectedReason=" << decision.noInputRejectedReason
                 << " localHazardWindowTicks=" << decision.localHazardWindowTicks
                 << " noJumpDeathTick=" << decision.noJumpDeathTick
                 << " noJumpDeathReason=" << decision.noJumpDeathReason
                 << " noJumpDeathObjectID=" << decision.noJumpHazardCollision.objectID
                 << " noJumpDeathObjectUID=" << decision.noJumpHazardCollision.uniqueID
                 << " noJumpDeathObjectCategory=" << cachedObjectCategoryName(decision.noJumpHazardCollision.category)
                 << " noJumpDeathLikelySpike=" << decision.noJumpHazardCollision.likelySpike
                 << " noJumpDeathApproximation=" << collisionApproximationName(decision.noJumpHazardCollision.approximation)
                 << " noJumpDeathCollisionType=" << decision.noJumpHazardCollision.collisionType
                 << " noJumpDeathObjectBounds=" << decision.noJumpHazardCollision.objectBounds.left << "," << decision.noJumpHazardCollision.objectBounds.right << "," << decision.noJumpHazardCollision.objectBounds.bottom << "," << decision.noJumpHazardCollision.objectBounds.top
                 << " noJumpDeathPlayerBounds=" << decision.noJumpHazardCollision.playerBounds.left << "," << decision.noJumpHazardCollision.playerBounds.right << "," << decision.noJumpHazardCollision.playerBounds.bottom << "," << decision.noJumpHazardCollision.playerBounds.top
                 << " tap0DeathTick=" << decision.tap0DeathTick
                 << " tap0DeathReason=" << decision.tap0DeathReason
                 << " tap0DeathObjectID=" << decision.tap0HazardCollision.objectID
                 << " tap0DeathObjectUID=" << decision.tap0HazardCollision.uniqueID
                 << " tap0DeathObjectCategory=" << cachedObjectCategoryName(decision.tap0HazardCollision.category)
                 << " tap0DeathLikelySpike=" << decision.tap0HazardCollision.likelySpike
                 << " tap0DeathApproximation=" << collisionApproximationName(decision.tap0HazardCollision.approximation)
                 << " tap0DeathCollisionType=" << decision.tap0HazardCollision.collisionType
                 << " tap0DeathObjectBounds=" << decision.tap0HazardCollision.objectBounds.left << "," << decision.tap0HazardCollision.objectBounds.right << "," << decision.tap0HazardCollision.objectBounds.bottom << "," << decision.tap0HazardCollision.objectBounds.top
                 << " tap0DeathPlayerBounds=" << decision.tap0HazardCollision.playerBounds.left << "," << decision.tap0HazardCollision.playerBounds.right << "," << decision.tap0HazardCollision.playerBounds.bottom << "," << decision.tap0HazardCollision.playerBounds.top
                 << " tap0SurvivesLocalHazard=" << decision.tap0SurvivesLocalHazard
                 << " tapZeroSurvivesFullHorizon=" << decision.tapZeroSurvivesFullHorizon
                 << " tap0ApexY=" << decision.tap0ApexY
                 << " tap0ApexTick=" << decision.tap0ApexTick
                 << " tap0LandedTick=" << decision.tap0LandedTick
                 << " selectedTap=" << decision.selectedTap
                 << " selectedTapLabel=" << decision.selectedTapLabel
                 << " selectedTapGrounded=" << decision.selectedTapGrounded
                 << " selectedTapCount=" << decision.selectedFutureTapCount + (decision.selectedTap >= 0 ? 1 : 0)
                 << " selectedRequiresFutureTap=" << decision.selectedRequiresFutureTap
                 << " futureTapCommitEnabled=" << decision.futureTapCommitEnabled
                 << " selectedFutureTapCount=" << decision.selectedFutureTapCount
                 << " selectedSecondTap=" << decision.selectedSecondTap
                 << " selectedRejectedReason=" << decision.selectedRejectedReason
                 << " selectedApexY=" << decision.selectedApexY
                 << " selectedApexTick=" << decision.selectedApexTick
                 << " selectedLandedTick=" << decision.selectedLandedTick
                 << " selectedDeathObjectID=" << decision.selectedHazardCollision.objectID
                 << " selectedDeathObjectUID=" << decision.selectedHazardCollision.uniqueID
                 << " selectedDeathObjectCategory=" << cachedObjectCategoryName(decision.selectedHazardCollision.category)
                 << " selectedDeathLikelySpike=" << decision.selectedHazardCollision.likelySpike
                 << " selectedDeathApproximation=" << collisionApproximationName(decision.selectedHazardCollision.approximation)
                 << " selectedDeathCollisionType=" << decision.selectedHazardCollision.collisionType
                 << " selectedDeathObjectBounds=" << decision.selectedHazardCollision.objectBounds.left << "," << decision.selectedHazardCollision.objectBounds.right << "," << decision.selectedHazardCollision.objectBounds.bottom << "," << decision.selectedHazardCollision.objectBounds.top
                 << " selectedDeathPlayerBounds=" << decision.selectedHazardCollision.playerBounds.left << "," << decision.selectedHazardCollision.playerBounds.right << "," << decision.selectedHazardCollision.playerBounds.bottom << "," << decision.selectedHazardCollision.playerBounds.top
                 << " selectedDecisionSource=" << decision.selectedDecisionSource
                 << " shouldPress=" << decision.shouldPress
                 << " planTimeMs=" << decision.planTimeMs
                 << " tickTimeMs=" << decision.tickTimeMs
                 << " horizonTicks=" << decision.horizonTicks
                 << " nearbyObjects=" << m_nearbyObjects.all.size()
                 << " nearbyHazards=" << decision.nearbyHazards
                 << " nearbySolids=" << decision.nearbySolids
                 << " nearbyInteractives=" << decision.nearbyInteractives
                 << " simulatedCandidateCount=" << decision.simulatedCandidateCount
                 << " simulatedStepCount=" << decision.simulatedStepCount
                 << " hazardChecks=" << decision.hazardChecks
                 << " solidChecks=" << decision.solidChecks
                 << " clearanceChecks=" << decision.clearanceChecks
                 << " earlyExitCount=" << decision.earlyExitCount
                 << " plannerBudgetExceeded=" << decision.plannerBudgetExceeded
                 << " usedBudgetFallback=" << decision.usedBudgetFallback
                 << " usedFastPath=" << decision.usedFastPath
                 << " expensivePlanningReason=" << decision.expensivePlanningReason
                 << " cacheObjects=" << m_levelCache.size()
                 << " candidates=" << decision.candidateCount
                 << " collision=" << collisionApproximationName(decision.collisionApproximation)
                 << " reason=" << decision.reason;
            auto lineText = line.str();
            AutoBotDebug::get()->bufferTickEvent(lineText);

            bool logInterestingTick = !m_hasLastLoggedDecision
                || decision.reason != m_lastLoggedReason
                || decision.shouldPress != m_lastLoggedShouldPress
                || decision.dropGateCheckedBeforeJumpSelection
                || decision.usedBudgetFallback
                || (snapshot.frame % 240) == 0;

            if (logInterestingTick) {
                AutoBotDebug::get()->logEvent("tick", lineText);
                m_hasLastLoggedDecision = true;
                m_lastLoggedReason = decision.reason;
                m_lastLoggedShouldPress = decision.shouldPress;
            }

            if (decision.usedBudgetFallback) {
                AutoBotDebug::get()->flushBufferedTickEvents("planner-budget-exceeded");
            }

            if (!std::isfinite(decision.nearestHazardDistance)
                && decision.noJumpDeathReason == "hazard"
                && decision.noJumpHazardCollision.valid) {
                std::ostringstream suspected;
                suspected << "frame=" << snapshot.frame
                          << " x=" << snapshot.x
                          << " y=" << snapshot.y
                          << " deathObjectID=" << decision.noJumpHazardCollision.objectID
                          << " deathObjectUID=" << decision.noJumpHazardCollision.uniqueID
                          << " deathCategory=" << cachedObjectCategoryName(decision.noJumpHazardCollision.category)
                          << " deathCollisionType=" << decision.noJumpHazardCollision.collisionType
                          << " deathObjectBounds=" << decision.noJumpHazardCollision.objectBounds.left << "," << decision.noJumpHazardCollision.objectBounds.right << "," << decision.noJumpHazardCollision.objectBounds.bottom << "," << decision.noJumpHazardCollision.objectBounds.top
                          << " deathPlayerBounds=" << decision.noJumpHazardCollision.playerBounds.left << "," << decision.noJumpHazardCollision.playerBounds.right << "," << decision.noJumpHazardCollision.playerBounds.bottom << "," << decision.noJumpHazardCollision.playerBounds.top;
                AutoBotDebug::get()->logEvent("suspected-false-hazard", suspected.str());
            }

            if (decision.planTimeMs > kPlannerSoftBudgetMs
                && (decision.usedBudgetFallback || snapshot.frame - m_lastPerfLogFrame >= 30)) {
                std::ostringstream perfLine;
                perfLine << "frame=" << snapshot.frame
                         << " planTimeMs=" << decision.planTimeMs
                         << " tickTimeMs=" << decision.tickTimeMs
                         << " candidateCount=" << decision.candidateCount
                         << " horizonTicks=" << decision.horizonTicks
                         << " nearbyObjects=" << m_nearbyObjects.all.size()
                         << " nearbyHazards=" << decision.nearbyHazards
                         << " nearbySolids=" << decision.nearbySolids
                         << " nearbyInteractives=" << decision.nearbyInteractives
                         << " simulatedCandidateCount=" << decision.simulatedCandidateCount
                         << " simulatedStepCount=" << decision.simulatedStepCount
                         << " hazardChecks=" << decision.hazardChecks
                         << " solidChecks=" << decision.solidChecks
                         << " clearanceChecks=" << decision.clearanceChecks
                         << " earlyExitCount=" << decision.earlyExitCount
                         << " reason=" << (decision.expensivePlanningReason.empty() ? decision.reason : decision.expensivePlanningReason);
                AutoBotDebug::get()->logEvent("planner-perf", perfLine.str());
                m_lastPerfLogFrame = snapshot.frame;
            }
        }

        if (decision.planTimeMs > kPlannerSoftBudgetMs) {
            log::warn(
                "[AutoBot] planner slow | frame={} planTimeMs={:.3f} tickTimeMs={:.3f} candidateCount={} horizonTicks={} nearbyObjects={} nearbyHazards={} nearbySolids={} nearbyInteractives={} reason={}",
                snapshot.frame,
                decision.planTimeMs,
                decision.tickTimeMs,
                decision.candidateCount,
                decision.horizonTicks,
                m_nearbyObjects.all.size(),
                decision.nearbyHazards,
                decision.nearbySolids,
                decision.nearbyInteractives,
                decision.expensivePlanningReason.empty() ? decision.reason : decision.expensivePlanningReason
            );
        }
    }

    void resetState() {
        if (m_validation.active) {
            finishValidation();
        }

        m_holding = false;
        m_holdFramesRemaining = 0;
        m_nearbyObjects.clear();
        m_stateReader.reset();
        m_planner.reset();
        m_lastDecision = {};
        m_lastDecisionReason = "reset-state";
        m_hasLastLoggedDecision = false;
        m_lastLoggedReason.clear();
        m_lastLoggedShouldPress = false;
        m_lastPerfLogFrame = std::numeric_limits<int>::min() / 2;
        m_validation = {};
        m_validationRunsStarted = 0;

        if (zBot::get()->autoBotFileLogging) {
            AutoBotDebug::get()->clearBufferedTickEvents();
            AutoBotDebug::get()->logEvent("reset-state", "holding=0 runtime-state-reset");
        }
    }

    void invalidateLevelCache() {
        m_levelCache.invalidate();
    }

    void warmupLevel(GJBaseGameLayer* gl, float dt = 0.f) {
        (void)dt;
        if (!gl || !gl->m_player1 || gl->m_player1->m_isDead) {
            return;
        }

        ensureLevelCache(gl);
        auto snapshot = m_stateReader.capture(gl, gl->m_player1, m_holding, dt);
        collectNearbyObjects(snapshot, currentPlannerConfig());
    }

    std::string const& getLastDecisionReason() const {
        return m_lastDecisionReason;
    }

    size_t getCachedObjectCount() const {
        return m_levelCache.size();
    }

    size_t getNearbyObjectCount() const {
        return m_nearbyObjects.all.size();
    }

private:
    struct PhysicsValidationSession {
        bool active = false;
        int maxTicks = 0;
        int landingTickReal = -1;
        int apexTickReal = -1;
        float apexYReal = 0.f;
        CubeSimulationTrace simulated;
        std::vector<CubeTraceSample> realSamples;
    };

    bool m_holding = false;
    int m_holdFramesRemaining = 0;
    size_t m_tickCounter = 0;
    std::string m_lastDecisionReason = "startup";
    PlannerDecision m_lastDecision;
    StateReader m_stateReader;
    LevelObjectCache m_levelCache;
    RollingLookaheadPlanner m_planner;
    NearbyObjectSet m_nearbyObjects;
    bool m_hasLastLoggedDecision = false;
    std::string m_lastLoggedReason;
    bool m_lastLoggedShouldPress = false;
    int m_lastPerfLogFrame = std::numeric_limits<int>::min() / 2;
    PhysicsValidationSession m_validation;
    int m_validationRunsStarted = 0;

    PlannerConfig currentPlannerConfig() const {
        PlannerConfig config;
        config.experimentalMultiMode = zBot::get()->autoBotExperimentalMultiMode;
        config.useApproximateCollisionFallback = zBot::get()->autoBotApproximateCollisionFallback;
        config.horizonSeconds = kRollingCubePlannerHorizonSeconds;
        config.ticksPerSecond = static_cast<float>(zBot::get()->tps);
        config.cubeTimingSafetyTicks = std::clamp(zBot::get()->autoBotCubeTimingSafetyTicks, 0, kMaxCubeTimingSafetyTicks);
        return config;
    }

    void beginValidation(PlayerSnapshot const& snapshot, PlannerConfig const& config, PlannerDecision const& decision) {
        if (m_validation.active) {
            return;
        }

        int horizonTicks = std::min(160, std::max(120, decision.horizonTicks));
        m_validation.active = true;
        m_validation.maxTicks = horizonTicks;
        m_validation.landingTickReal = -1;
        m_validation.apexTickReal = 0;
        m_validation.apexYReal = snapshot.y;
        m_validation.realSamples.clear();
        m_validation.simulated = RollingLookaheadPlanner::traceCubeCandidate(
            snapshot,
            m_nearbyObjects,
            config,
            CubeCandidate { { 0 }, false, "physics-validation" },
            horizonTicks,
            false
        );
        m_validation.realSamples.push_back(CubeTraceSample { 0, snapshot.x, snapshot.y, snapshot.yVelocity, snapshot.onGround });
        ++m_validationRunsStarted;

        if (zBot::get()->autoBotFileLogging) {
            std::ostringstream line;
            line << "frame=" << snapshot.frame
                 << " mode=" << gameModeName(snapshot.mode)
                 << " x=" << snapshot.x
                 << " y=" << snapshot.y
                 << " yVel=" << snapshot.yVelocity
                 << " onGround=" << snapshot.onGround
                 << " gravFlip=" << snapshot.gravFlip
                 << " xSpeed=" << snapshot.observedXSpeed
                 << " inputHeld=" << snapshot.inputHeld
                 << " maxTicks=" << horizonTicks
                 << " simApexY=" << m_validation.simulated.result.apexY
                 << " simApexTick=" << m_validation.simulated.result.apexTick
                 << " simLandedTick=" << m_validation.simulated.result.landedTick;
            AutoBotDebug::get()->logEvent("physics-validation-start", line.str());
        }
    }

    void finishValidation() {
        if (!m_validation.active) {
            return;
        }

        auto const& real = m_validation.realSamples;
        auto const& sim = m_validation.simulated.samples;
        std::size_t compareCount = std::min(real.size(), sim.size());
        if (compareCount == 0) {
            m_validation = {};
            return;
        }
        float maxXError = 0.f;
        float maxYError = 0.f;
        float maxYVelError = 0.f;
        int onGroundMismatchCount = 0;

        for (std::size_t i = 0; i < compareCount; ++i) {
            maxXError = std::max(maxXError, std::abs(real[i].x - sim[i].x));
            maxYError = std::max(maxYError, std::abs(real[i].y - sim[i].y));
            maxYVelError = std::max(maxYVelError, std::abs(real[i].yVelocity - sim[i].yVelocity));
            if (real[i].onGround != sim[i].onGround) {
                ++onGroundMismatchCount;
            }
        }

        if (zBot::get()->autoBotFileLogging) {
            int landingTickError = (m_validation.landingTickReal >= 0 && m_validation.simulated.result.landedTick >= 0)
                ? (m_validation.landingTickReal - m_validation.simulated.result.landedTick)
                : -1;

            std::ostringstream summary;
            summary << "samples=" << compareCount
                    << " maxXError=" << maxXError
                    << " maxYError=" << maxYError
                    << " maxYVelError=" << maxYVelError
                    << " onGroundMismatchCount=" << onGroundMismatchCount
                    << " landingTickReal=" << m_validation.landingTickReal
                    << " landingTickSim=" << m_validation.simulated.result.landedTick
                    << " landingTickError=" << landingTickError
                    << " apexYReal=" << m_validation.apexYReal
                    << " apexYSim=" << m_validation.simulated.result.apexY
                    << " apexTickReal=" << m_validation.apexTickReal
                    << " apexTickSim=" << m_validation.simulated.result.apexTick;
            AutoBotDebug::get()->logEvent("physics-validation-summary", summary.str());
        }

        m_validation = {};
    }

    void updateValidation(PlayerSnapshot const& snapshot) {
        if (!m_validation.active) {
            return;
        }

        int tickOffset = static_cast<int>(m_validation.realSamples.size());
        if (tickOffset > m_validation.maxTicks) {
            finishValidation();
            return;
        }

        bool previousOnGround = m_validation.realSamples.empty() ? snapshot.onGround : m_validation.realSamples.back().onGround;
        m_validation.realSamples.push_back(CubeTraceSample {
            tickOffset,
            snapshot.x,
            snapshot.y,
            snapshot.yVelocity,
            snapshot.onGround,
        });

        if ((!snapshot.gravFlip && snapshot.y > m_validation.apexYReal)
            || (snapshot.gravFlip && snapshot.y < m_validation.apexYReal)
            || tickOffset == 0) {
            m_validation.apexYReal = snapshot.y;
            m_validation.apexTickReal = tickOffset;
        }

        if (!previousOnGround && snapshot.onGround && m_validation.landingTickReal < 0) {
            m_validation.landingTickReal = tickOffset;
        }

        if (zBot::get()->autoBotFileLogging
            && !m_validation.simulated.samples.empty()
            && tickOffset > 0
            && (tickOffset % 5) == 0) {
            std::size_t simIndex = std::min<std::size_t>(static_cast<std::size_t>(tickOffset), m_validation.simulated.samples.size() - 1);
            auto const& sim = m_validation.simulated.samples[simIndex];
            std::ostringstream line;
            line << "tickOffset=" << tickOffset
                 << " realX=" << snapshot.x
                 << " realY=" << snapshot.y
                 << " realYVel=" << snapshot.yVelocity
                 << " realOnGround=" << snapshot.onGround
                 << " simX=" << sim.x
                 << " simY=" << sim.y
                 << " simYVel=" << sim.yVelocity
                 << " simOnGround=" << sim.onGround
                 << " xError=" << (snapshot.x - sim.x)
                 << " yError=" << (snapshot.y - sim.y)
                 << " yVelError=" << (snapshot.yVelocity - sim.yVelocity)
                 << " onGroundMismatch=" << (snapshot.onGround != sim.onGround);
            AutoBotDebug::get()->logEvent("physics-validation-sample", line.str());
        }

        if (tickOffset >= m_validation.maxTicks
            || (m_validation.landingTickReal >= 0 && tickOffset >= m_validation.landingTickReal + 4)) {
            finishValidation();
        }
    }

    void ensureLevelCache(GJBaseGameLayer* gl) {
        if (m_levelCache.isBuilt()) {
            return;
        }

        if (m_levelCache.build(gl, zBot::get()->autoBotLogUnknownObjects) && zBot::get()->autoBotFileLogging) {
            std::ostringstream line;
            line << "objects=" << m_levelCache.size();
            AutoBotDebug::get()->logEvent("level-cache", line.str());
        }
    }

    void collectNearbyObjects(PlayerSnapshot const& snapshot, PlannerConfig const& config) {
        int horizonTicks = std::clamp(
            static_cast<int>(std::round(config.ticksPerSecond * std::max(config.horizonSeconds, 0.5f))),
            45,
            360
        );
        float lookahead = std::max(snapshot.observedXSpeed * static_cast<float>(horizonTicks) + 240.f, 480.f);
        m_levelCache.queryCategorizedRange(snapshot.x - 80.f, snapshot.x + lookahead, m_nearbyObjects);
    }

    PlannerDecision decide(PlayerSnapshot const& snapshot, PlannerConfig const& config) {
        switch (snapshot.mode) {
            case GameMode::Cube:
                return m_planner.planCube(snapshot, m_nearbyObjects, config);

            default:
                return m_planner.planUnsupported(snapshot, config);
        }
    }

    void applyDecision(GJBaseGameLayer* gl, PlayerSnapshot const& snapshot, bool shouldPress) {
        if (shouldPress && !m_holding) {
            m_holdFramesRemaining = desiredHoldFrames(snapshot.mode);
            gl->handleButton(true, static_cast<int>(PlayerButton::Jump), true);
            m_holding = true;
            return;
        }

        if (!shouldPress && m_holding) {
            if (m_holdFramesRemaining > 0) {
                --m_holdFramesRemaining;
                return;
            }

            gl->handleButton(false, static_cast<int>(PlayerButton::Jump), true);
            m_holding = false;
            m_holdFramesRemaining = 0;
        }
    }

    static int desiredHoldFrames(GameMode mode) {
        switch (mode) {
            case GameMode::Cube:
                return 0;
            case GameMode::Robot:
                return 4;
            default:
                return 1;
        }
    }
};

} // namespace autobot

#endif
