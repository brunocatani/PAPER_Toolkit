#include "redux/WeaponPartMotionLearner.h"

#include "ReduxLog.h"

#include <algorithm>
#include <cstring>

namespace redux
{
    namespace
    {
        std::string_view slotName(const std::array<char, WeaponPartMotionLearner::kMaxSourceName>& name)
        {
            std::size_t length = 0;
            while (length < name.size() && name[length] != '\0') {
                ++length;
            }
            return std::string_view(name.data(), length);
        }

        void copySlotName(std::array<char, WeaponPartMotionLearner::kMaxSourceName>& target, std::string_view source)
        {
            target = {};
            const auto count = (std::min)(source.size(), target.size() - 1);
            std::memcpy(target.data(), source.data(), count);
        }

        bool slotMatches(
            std::uint32_t slotFormId,
            const std::array<char, WeaponPartMotionLearner::kMaxSourceName>& name,
            std::uint32_t weaponFormId,
            std::string_view sourceName)
        {
            return slotFormId == weaponFormId && slotName(name) == sourceName;
        }
    }

    void WeaponPartMotionLearner::beginObservationFrame()
    {
        ++_frameCounter;
    }

    void WeaponPartMotionLearner::observe(const Observation& observation)
    {
        if (observation.weaponFormId == 0 || observation.sourceName.empty()) {
            return;
        }
        ++_observationCounter;

        auto* recorder = acquireRecorderSlot(observation.weaponFormId, observation.sourceName);
        if (!recorder) {
            return;
        }
        recorder->lastSeenCounter = _observationCounter;

        const bool wasRecording = recorder->state.phase == weapon_part_motion_path::RecorderPhase::Recording;
        const auto result = weapon_part_motion_path::step(
            recorder->state,
            recorder->buffer.data(),
            observation.pose,
            observation.trusted);
        if (!wasRecording && recorder->state.phase == weapon_part_motion_path::RecorderPhase::Recording) {
            // buffer[1] was written this frame; buffer[0] is the pre-motion
            // rest pose.
            recorder->startFrame = _frameCounter;
        }
        if (result == weapon_part_motion_path::StepResult::RecordingComplete) {
            storeCompletedPath(*recorder);
        }
    }

    const WeaponPartMotionLearner::PathRecord* WeaponPartMotionLearner::selectRecord(
        const PathSlot& slot,
        MotionPathMode mode)
    {
        switch (mode) {
        case MotionPathMode::AuthoredOnly:
            return slot.authored.used ? &slot.authored : nullptr;
        case MotionPathMode::LearnedOnly:
            return slot.learned.used ? &slot.learned : nullptr;
        case MotionPathMode::Hybrid:
        default:
            // Learner priority (Bruno, 2026-07-04): a real observed stroke
            // outranks authored clip data; authored fills the gap until the
            // part is taught.
            if (slot.learned.used) {
                return &slot.learned;
            }
            return slot.authored.used ? &slot.authored : nullptr;
        }
    }

    const WeaponPartMotionLearner::PathSlot* WeaponPartMotionLearner::findSlot(
        std::uint32_t weaponFormId,
        std::string_view sourceName) const
    {
        for (const auto& slot : _paths) {
            if (slot.used && slotMatches(slot.weaponFormId, slot.sourceName, weaponFormId, sourceName)) {
                return &slot;
            }
        }
        return nullptr;
    }

    const weapon_part_motion_path::MotionPath* WeaponPartMotionLearner::findPath(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        MotionPathMode mode) const
    {
        // lastUseCounter is an eviction hint, not behavior; keeping this
        // accessor const outweighs refreshing it on reads.
        const auto* slot = findSlot(weaponFormId, sourceName);
        const auto* record = slot ? selectRecord(*slot, mode) : nullptr;
        return record ? &record->path : nullptr;
    }

    WeaponPartMotionLearner::GroupView WeaponPartMotionLearner::findGroup(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        MotionPathMode mode) const
    {
        const auto* slot = findSlot(weaponFormId, sourceName);
        const auto* record = slot ? selectRecord(*slot, mode) : nullptr;
        if (!record) {
            return {};
        }
        return GroupView{
            .leaderPath = &record->path,
            .followers = record->followers.data(),
            .followerCount = record->followerCount,
            .authored = record == &slot->authored,
            .fallbackSource = record == &slot->authored && record->fallbackSource,
        };
    }

    WeaponPartMotionLearner::SourceAvailability WeaponPartMotionLearner::sourceAvailability(
        std::uint32_t weaponFormId,
        std::string_view sourceName) const
    {
        const auto* slot = findSlot(weaponFormId, sourceName);
        if (!slot) {
            return {};
        }
        return SourceAvailability{
            .learned = slot->learned.used,
            .authored = slot->authored.used,
            .authoredFallback = slot->authored.used && slot->authored.fallbackSource,
        };
    }

    WeaponPartMotionLearner::PathSlot* WeaponPartMotionLearner::findOrClaimSlot(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        bool preferSlotsWithoutLearnedData)
    {
        PathSlot* freeSlot = nullptr;
        PathSlot* preferredEviction = nullptr;
        PathSlot* anyEviction = nullptr;
        for (auto& slot : _paths) {
            if (slot.used && slotMatches(slot.weaponFormId, slot.sourceName, weaponFormId, sourceName)) {
                return &slot;
            }
            if (!slot.used) {
                freeSlot = freeSlot ? freeSlot : &slot;
                continue;
            }
            if (!anyEviction || slot.lastUseCounter < anyEviction->lastUseCounter) {
                anyEviction = &slot;
            }
            if (preferSlotsWithoutLearnedData && slot.learned.used) {
                continue;
            }
            if (!preferredEviction || slot.lastUseCounter < preferredEviction->lastUseCounter) {
                preferredEviction = &slot;
            }
        }

        auto* claimed = freeSlot ? freeSlot : (preferredEviction ? preferredEviction : anyEviction);
        if (!claimed) {
            return nullptr;
        }
        if (claimed->used) {
            RDX_LOG_DEBUG(Weapon,
                "WeaponPartMotionLearner: evicting path slot for part '{}' on weapon {:08X} (learned={} authored={})",
                slotName(claimed->sourceName),
                claimed->weaponFormId,
                claimed->learned.used,
                claimed->authored.used);
        }
        *claimed = {};
        claimed->used = true;
        claimed->weaponFormId = weaponFormId;
        copySlotName(claimed->sourceName, sourceName);
        return claimed;
    }

    void WeaponPartMotionLearner::storeAuthoredGroup(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        const weapon_clip_stroke::AuthoredStrokeGroup& group,
        bool fallbackSource)
    {
        if (weaponFormId == 0 || sourceName.empty() || !group.leaderPath.valid) {
            return;
        }
        ++_observationCounter;

        auto* target = findOrClaimSlot(weaponFormId, sourceName, true);
        if (!target) {
            return;
        }
        auto& record = target->authored;
        if (record.used) {
            // SOURCE TIER (Bruno, 2026-07-04): the data must be what THIS
            // weapon actually plays — a stroke from a clip the weapon
            // ACTIVATED always beats a merely-loaded fallback stroke for the
            // same part; fallback data only stands while nothing else
            // exists. Within the same tier the largest leader stroke wins
            // (a reload stroke beats a fire nudge).
            if (record.fallbackSource != fallbackSource) {
                if (fallbackSource) {
                    return;
                }
            } else if (!weapon_part_motion_path::shouldReplacePath(record.path, group.leaderPath)) {
                return;
            }
        }

        const bool replaced = record.used;
        record.used = true;
        record.fallbackSource = fallbackSource;
        record.path = group.leaderPath;
        record.followerCount = (std::min)(group.followerCount, static_cast<std::uint32_t>(record.followers.size()));
        record.followers = group.followers;
        target->lastUseCounter = _observationCounter;
        ++_revision;

        // Path endpoints in the stored frame: comparing these against the
        // learner's endpoints for the same part exposes any frame mismatch
        // between authored (rig-derived) and learned (scene-observed) data.
        const auto& firstKey = group.leaderPath.keys.front();
        const auto& lastKey = group.leaderPath.keys.back();
        RDX_LOG_INFO(Weapon,
            "WeaponPartMotionLearner: {} AUTHORED [{}] stroke group for part '{}' on weapon {:08X} (leader arc {:.2f} game units, {} followers, learned record {}) start=({:.2f},{:.2f},{:.2f}) end=({:.2f},{:.2f},{:.2f})",
            replaced ? "updated" : "stored",
            fallbackSource ? "fallback-loaded" : "weapon-clip",
            sourceName,
            weaponFormId,
            group.leaderPath.totalArcLength,
            record.followerCount,
            target->learned.used ? "also present" : "absent",
            firstKey.translate.x,
            firstKey.translate.y,
            firstKey.translate.z,
            lastKey.translate.x,
            lastKey.translate.y,
            lastKey.translate.z);
    }

    void WeaponPartMotionLearner::reset()
    {
        /*
         * Slot-by-slot on purpose: `_paths = {}` / `_recorders = {}`
         * materialize full temporary ARRAYS on the stack (~1MB each with
         * dual-record slots), and both temporaries share one frame — this
         * overflowed the game main thread's stack in the load-game reset
         * (in-game 2026-07-04, EXCEPTION_STACK_OVERFLOW in __chkstk).
         * Per-slot temporaries keep the frame at one slot (~20KB).
         */
        for (auto& slot : _paths) {
            slot = {};
        }
        for (auto& slot : _recorders) {
            slot = {};
        }
        _observationCounter = 0;
        ++_revision;
    }

    WeaponPartMotionLearner::RecorderSlot* WeaponPartMotionLearner::acquireRecorderSlot(
        std::uint32_t weaponFormId,
        std::string_view sourceName)
    {
        RecorderSlot* freeSlot = nullptr;
        RecorderSlot* staleSlot = nullptr;
        for (auto& slot : _recorders) {
            if (slot.used && slotMatches(slot.weaponFormId, slot.sourceName, weaponFormId, sourceName)) {
                return &slot;
            }
            if (!slot.used) {
                freeSlot = freeSlot ? freeSlot : &slot;
            } else if (_observationCounter - slot.lastSeenCounter > kRecorderStaleObservationAge &&
                       (!staleSlot || slot.lastSeenCounter < staleSlot->lastSeenCounter)) {
                staleSlot = &slot;
            }
        }

        auto* claimed = freeSlot ? freeSlot : staleSlot;
        if (!claimed) {
            return nullptr;
        }
        claimed->used = true;
        claimed->weaponFormId = weaponFormId;
        copySlotName(claimed->sourceName, sourceName);
        claimed->lastSeenCounter = _observationCounter;
        claimed->state = {};
        return claimed;
    }

    void WeaponPartMotionLearner::storeCompletedPath(const RecorderSlot& recorder)
    {
        weapon_part_motion_path::MotionPath candidate{};
        std::array<float, weapon_part_motion_path::kResampledKeyCount> keyPositions{};
        if (!weapon_part_motion_path::buildPathFromRecording(
                recorder.buffer.data(), recorder.state.sampleCount, candidate, keyPositions.data())) {
            return;
        }

        /*
         * Co-movement grouping (Bruno, 2026-07-04): parts whose distance to
         * this stroke's part stayed constant while both were moving traveled
         * as one rigid assembly (a magazine and its bullets, a slide and its
         * sights) — the same signal the authored clips encode as followers,
         * recovered here from observation. Concurrent recordings are
         * frame-aligned via startFrame; followers are resampled at the same
         * source positions as the leader's keys so they stay time-locked.
         */
        struct FollowerCandidate
        {
            const RecorderSlot* recorder{ nullptr };
            std::int64_t frameOffset{ 0 };
        };
        std::array<FollowerCandidate, weapon_clip_stroke::kMaxFollowers> followerCandidates{};
        std::uint32_t followerCandidateCount = 0;
        constexpr float kRigidDistanceToleranceGameUnits =
            weapon_clip_stroke::kRigidFollowerDistanceToleranceGameUnits;
        constexpr std::int64_t kMinOverlapSamples = 8;
        for (const auto& other : _recorders) {
            if (&other == &recorder || !other.used || other.weaponFormId != recorder.weaponFormId ||
                other.state.sampleCount < 2 ||
                followerCandidateCount >= followerCandidates.size()) {
                continue;
            }
            // Frame f holds leader sample f-startFrame+1 and other sample
            // f-other.startFrame+1 (index 0 is each recording's rest pose).
            const auto offset =
                static_cast<std::int64_t>(recorder.startFrame) - static_cast<std::int64_t>(other.startFrame);
            float minDistance = 0.0f;
            float maxDistance = 0.0f;
            std::int64_t overlap = 0;
            std::int64_t firstOtherIndex = -1;
            std::int64_t lastOtherIndex = -1;
            for (std::uint32_t i = 1; i < recorder.state.sampleCount; ++i) {
                const auto otherIndex = static_cast<std::int64_t>(i) + offset;
                if (otherIndex < 1 || otherIndex >= static_cast<std::int64_t>(other.state.sampleCount)) {
                    continue;
                }
                const float distance = weapon_part_motion_path::length(weapon_part_motion_path::sub(
                    recorder.buffer[i].translate,
                    other.buffer[static_cast<std::size_t>(otherIndex)].translate));
                if (overlap == 0) {
                    minDistance = distance;
                    maxDistance = distance;
                    firstOtherIndex = otherIndex;
                } else {
                    minDistance = (std::min)(minDistance, distance);
                    maxDistance = (std::max)(maxDistance, distance);
                }
                lastOtherIndex = otherIndex;
                ++overlap;
            }
            if (overlap < kMinOverlapSamples || maxDistance - minDistance > kRigidDistanceToleranceGameUnits) {
                continue;
            }
            // Constant distance to a purely ROTATING leader does not prove
            // co-movement; the follower itself must have traveled.
            if (weapon_part_motion_path::poseDistance(
                    other.buffer[static_cast<std::size_t>(firstOtherIndex)],
                    other.buffer[static_cast<std::size_t>(lastOtherIndex)]) <
                weapon_clip_stroke::kFollowerMinExcursionGameUnits) {
                continue;
            }
            followerCandidates[followerCandidateCount++] = FollowerCandidate{ &other, offset };
        }

        auto* target = findOrClaimSlot(recorder.weaponFormId, slotName(recorder.sourceName), false);
        if (!target) {
            return;
        }
        auto& record = target->learned;
        // Between learned strokes the larger stroke wins; the authored
        // record is untouched either way (dual storage — see header).
        if (record.used && !weapon_part_motion_path::shouldReplacePath(record.path, candidate)) {
            return;
        }

        const bool replaced = record.used;
        record.used = true;
        record.fallbackSource = false;
        record.path = candidate;
        target->lastUseCounter = _observationCounter;
        ++_revision;

        // Followers: the rigid co-movers, resampled at the leader's key
        // positions (frame-aligned) so they replay time-locked to the stroke.
        record.followerCount = 0;
        for (std::uint32_t c = 0; c < followerCandidateCount &&
             record.followerCount < static_cast<std::uint32_t>(record.followers.size());
             ++c) {
            const auto& followerRecorder = *followerCandidates[c].recorder;
            const auto offset = followerCandidates[c].frameOffset;
            auto& slot = record.followers[record.followerCount];
            slot = {};
            slot.boneName = followerRecorder.sourceName;
            slot.restScale = 1.0f;
            const auto lastIndex = static_cast<float>(followerRecorder.state.sampleCount - 1);
            for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
                const float position = (std::min)(
                    lastIndex,
                    (std::max)(0.0f, keyPositions[key] + static_cast<float>(offset)));
                const auto base = static_cast<std::size_t>(position);
                const float t = position - static_cast<float>(base);
                const auto next = (std::min)(base + 1, static_cast<std::size_t>(lastIndex));
                slot.keys[key] = weapon_part_motion_path::lerpPose(
                    followerRecorder.buffer[base],
                    followerRecorder.buffer[next],
                    t);
            }
            ++record.followerCount;
        }

        // Once per completed stroke, never per frame. Path endpoints in the
        // stored frame — the learned counterpart of the AUTHORED endpoint
        // log, for frame-mismatch comparison between the two sources.
        const auto& firstKey = candidate.keys.front();
        const auto& lastKey = candidate.keys.back();
        RDX_LOG_INFO(Weapon,
            "WeaponPartMotionLearner: {} LEARNED motion path for part '{}' on weapon {:08X} (stroke arc {:.2f} game units, {} keys, {} raw samples, {} co-moving followers, authored record {}) start=({:.2f},{:.2f},{:.2f}) end=({:.2f},{:.2f},{:.2f})",
            replaced ? "updated" : "learned",
            slotName(recorder.sourceName),
            recorder.weaponFormId,
            candidate.totalArcLength,
            weapon_part_motion_path::kResampledKeyCount,
            recorder.state.sampleCount,
            record.followerCount,
            target->authored.used ? "also present" : "absent",
            firstKey.translate.x,
            firstKey.translate.y,
            firstKey.translate.z,
            lastKey.translate.x,
            lastKey.translate.y,
            lastKey.translate.z);
    }
}
