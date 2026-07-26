#pragma once

#include <chrono>

struct Actor;
struct World;
struct TransportService;
struct UpdateEvent;
struct ProjectileLaunchedEvent;
struct NotifyProjectileLaunch;
struct HitEvent;

/**
 * @brief Responsible for projectiles, combat agro, etc.
 */
struct CombatService
{
    CombatService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~CombatService() noexcept = default;

    TP_NOCOPYMOVE(CombatService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnLocalComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnProjectileLaunchedEvent(const ProjectileLaunchedEvent& acEvent) const noexcept;
    void OnNotifyProjectileLaunch(const NotifyProjectileLaunch& acMessage) const noexcept;
    void OnHitEvent(const HitEvent& acEvent) noexcept;

    void RunTargetUpdates(const float acDelta) const noexcept;

    /**
     * @brief Put the local player and a remote player in mutual combat.
     *
     * The game only draws a health bar for actors it considers hostile, and
     * remote players sit in the player faction. Entering real combat makes the
     * vanilla HUD (and any health bar mod) treat them like any other opponent,
     * without touching factions or the respawn/bleedout behaviour.
     */
    void EnterPvpCombat(Actor* apLocal, Actor* apRemote) noexcept;

    /**
     * @brief Drop PvP combat once it is over.
     *
     * Ends when either side dies, when neither dealt damage for a while, or
     * when both have sheathed their weapons.
     */
    void RunPvpCombatUpdates() noexcept;

private:
    World& m_world;
    TransportService& m_transport;

    struct PvpEngagement
    {
        std::chrono::steady_clock::time_point lastDamage;
    };

    // Keyed by the remote player's form id.
    TiltedPhoques::Map<uint32_t, PvpEngagement> m_pvpEngagements;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_localComponentRemoved;
    entt::scoped_connection m_projectileLaunchedConnection;
    entt::scoped_connection m_projectileLaunchConnection;
    entt::scoped_connection m_hitConnection;
};
